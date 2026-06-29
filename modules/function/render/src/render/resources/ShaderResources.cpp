#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/description/Shader.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <algorithm>

namespace lux::render
{

namespace
{
    /// FNV-1a 64-bit over the raw SPIR-V bytes. Only a bucket key — exact
    /// byte-compare on hit guards against collisions, so this need not be strong.
    std::size_t hashSpirv(std::span<const std::byte> bytes) noexcept
    {
        std::size_t h = 1469598103934665603ull;
        for (std::byte b : bytes)
        {
            h ^= static_cast<std::size_t>(static_cast<unsigned char>(b));
            h *= 1099511628211ull;
        }
        return h;
    }
} // namespace

ShaderResources::~ShaderResources()
{
    if (initialized_)
        shutdown();
}

void ShaderResources::init(const InitInfo& info)
{
    device_ = info.device;
    records_.clear();
    gens_.clear();
    refcount_.clear();
    slot_hash_.clear();
    free_.clear();
    spirv_cache_.clear();
    initialized_ = true;
}

void ShaderResources::shutdown()
{
    if (!initialized_) return;
    initialized_ = false;

    // Destroy all live VkShaderModules
    for (size_t i = 0; i < records_.size(); ++i)
    {
        if (records_[i].module != VK_NULL_HANDLE && device_)
        {
            vkDestroyShaderModule(device_, records_[i].module, nullptr);
            records_[i].module = VK_NULL_HANDLE;
        }
    }
    records_.clear();
    gens_.clear();
    refcount_.clear();
    slot_hash_.clear();
    free_.clear();
    spirv_cache_.clear();
    device_ = VK_NULL_HANDLE;
}

ShaderHandle ShaderResources::add(const lux::rdesc::Shader& spirv, const lux::rdesc::ShaderInfo& info)
{
    return add(std::span<const std::byte>(
        static_cast<const std::byte*>(spirv.data()), spirv.size()), info);
}

ShaderHandle ShaderResources::add(std::span<const std::byte> spirv_bytes, const lux::rdesc::ShaderInfo& info)
{
    // ── Dedup: identical SPIR-V → identical handle (one shared VkShaderModule) ──
    // Materials/instances that compile to byte-identical SPIR-V collapse onto one
    // slot, so downstream the shader_key (= handle bits) and thus the PSO are
    // shared. Each hit bumps the refcount; the module dies only at the last
    // remove(). Hash buckets the lookup; an exact byte-compare guards collisions.
    // hashSpirv can legitimately return 0, but slot_hash_[idx]==0 is overloaded as
    // the "no dedup hash" sentinel — a real 0 hash made remove() skip cache
    // eviction, leaving a stale rec that aliases a freed slot. Normalize to a
    // non-zero bucket key; the hash only buckets (an exact byte-compare guards
    // collisions, below), so remapping 0->1 is safe. (C10)
    std::size_t h = hashSpirv(spirv_bytes);
    if (h == 0) h = 1;
    if (auto it = spirv_cache_.find(h); it != spirv_cache_.end())
    {
        for (const SpirvCacheRec& rec : it->second)
        {
            const uint32_t s = rec.slot;
            if (s < records_.size()
                && records_[s].module != VK_NULL_HANDLE
                && rec.bytes.size() == spirv_bytes.size()
                && std::equal(rec.bytes.begin(), rec.bytes.end(), spirv_bytes.begin()))
            {
                ++refcount_[s];
                return ShaderHandle{s, gens_[s]};
            }
        }
    }

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv_bytes.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(spirv_bytes.data());

    VkShaderModule sm{VK_NULL_HANDLE};
    if (vkCreateShaderModule(device_, &ci, nullptr, &sm) != VK_SUCCESS)
        return ShaderHandle::invalid();

    const ShaderHandle handle = add(ShaderObject{sm, info});
    if (!handle.is_null())
    {
        const uint32_t s = handle.index;
        slot_hash_[s] = h;
        spirv_cache_[h].push_back(SpirvCacheRec{
            std::vector<std::byte>(spirv_bytes.begin(), spirv_bytes.end()), s});
    }
    return handle;
}

ShaderHandle ShaderResources::add(ShaderObject obj)
{
    uint32_t idx;
    if (!free_.empty())
    {
        idx = free_.back();
        free_.pop_back();
        records_[idx]   = std::move(obj);
        refcount_[idx]  = 1;
        slot_hash_[idx] = 0;
    }
    else
    {
        idx = static_cast<uint32_t>(records_.size());
        records_.push_back(std::move(obj));
        gens_.push_back(1);
        refcount_.push_back(1);
        slot_hash_.push_back(0);
    }
    return ShaderHandle{idx, gens_[idx]};
}

const ShaderObject* ShaderResources::get(ShaderHandle handle) const noexcept
{
    if (handle.is_null())
        return nullptr;
    const auto idx = handle.index;
    if (idx >= static_cast<uint32_t>(records_.size()))
        return nullptr;
    if (handle.gen != gens_[idx])
        return nullptr;
    if (records_[idx].module == VK_NULL_HANDLE)
        return nullptr;
    return &records_[idx];
}

void ShaderResources::remove(ShaderHandle handle)
{
    if (handle.is_null())
        return;
    const auto idx = handle.index;
    if (idx >= static_cast<uint32_t>(records_.size()))
        return;
    if (handle.gen != gens_[idx])
        return;
    if (refcount_[idx] == 0)         // already dead
        return;
    if (--refcount_[idx] != 0)       // still shared by another material/instance
        return;

    auto& rec = records_[idx];
    if (rec.module != VK_NULL_HANDLE && device_)
    {
        vkDestroyShaderModule(device_, rec.module, nullptr);
        rec = ShaderObject{};
    }

    // Evict this slot's dedup-cache entry so a future identical SPIR-V re-creates
    // a fresh module rather than aliasing the freed slot.
    if (slot_hash_[idx] != 0)
    {
        if (auto it = spirv_cache_.find(slot_hash_[idx]); it != spirv_cache_.end())
        {
            auto& vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                          [idx](const SpirvCacheRec& r){ return r.slot == idx; }),
                      vec.end());
            if (vec.empty())
                spirv_cache_.erase(it);
        }
        slot_hash_[idx] = 0;
    }

    ++gens_[idx];
    free_.push_back(idx);
}

} // namespace lux::render
