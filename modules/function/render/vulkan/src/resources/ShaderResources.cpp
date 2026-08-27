#include <lux/engine/render/gpu/pipeline/SpirvPatcher.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <cstring>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/description/Shader.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <algorithm>
#include <ranges>

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
        sparse_instance_pages_ = info.sparse_instance_pages;
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
        if (!initialized_)
            return;
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
        sparse_instance_pages_ = false;
    }

    ShaderHandle ShaderResources::add(const lux::rdesc::Shader& spirv, const lux::rdesc::ShaderInfo& info)
    {
        return add(std::span<const std::byte>(static_cast<const std::byte*>(spirv.data()), spirv.size()), info);
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
        if (h == 0)
            h = 1;
        if (auto it = spirv_cache_.find(h); it != spirv_cache_.end())
        {
            for (const SpirvCacheRec& rec : it->second)
            {
                const uint32_t s = rec.slot;
                const bool has_valid_slot = s < records_.size();
                const bool has_live_module = has_valid_slot && records_[s].module != VK_NULL_HANDLE;
                const bool has_matching_size = has_live_module && rec.bytes.size() == spirv_bytes.size();
                const bool has_matching_bytes = has_matching_size &&
                    std::equal(rec.bytes.begin(), rec.bytes.end(), spirv_bytes.begin());
                if (has_matching_bytes)
                {
                    ++refcount_[s];
                    return ShaderHandle{s, gens_[s]};
                }
            }
        }

        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = spirv_bytes.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(spirv_bytes.data());

        VkShaderModule sm{VK_NULL_HANDLE};
        if (vkCreateShaderModule(device_, &ci, nullptr, &sm) != VK_SUCCESS)
            return ShaderHandle::invalid();

        const ShaderHandle handle = add(ShaderObject{sm, info});
        if (!handle.isNull())
        {
            const uint32_t s = handle.index;
            slot_hash_[s] = h;
            spirv_cache_[h].push_back(SpirvCacheRec{std::vector<std::byte>(spirv_bytes.begin(), spirv_bytes.end()), s});
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
            records_[idx] = std::move(obj);
            refcount_[idx] = 1;
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

    std::span<const std::byte> ShaderResources::spirvBytes(ShaderHandle handle) const noexcept
    {
        if (!get(handle))
            return {};
        const auto idx = handle.index;
        if (idx >= slot_hash_.size())
            return {};
        const std::size_t h = slot_hash_[idx];
        if (h == 0)
            return {}; // This slot wasn't added via the deduped SPIR-V add() path (a pre-built ShaderObject)

        const auto it = spirv_cache_.find(h);
        if (it == spirv_cache_.end())
            return {};
        for (const auto& rec : it->second)
            if (rec.slot == idx)
                return {rec.bytes.data(), rec.bytes.size()};
        return {};
    }

    Expected<ShaderHandle>
    ShaderResources::addMergedLayoutVariant(ShaderHandle source, const lux::rdesc::ShaderInfo& info)
    {
        const auto src_bytes = spirvBytes(source);
        if (src_bytes.empty())
            return renderFailure<err::shader::SourceBytesUnavailable>();

        // ── 门禁 1:一个 set 要么整体搬走,要么完全不搬 ────────────────────────
        //
        // 搬运按 binding 的名字逐条驱动,不在契约里的名字原样不动。所以一个引擎 set 若
        // 只有部分 binding 登记进了契约(例如 Light 只登记了 b0-b3,阴影段的 b4-b10 还
        // 没登记),搬完之后它会同时引用新旧两个 set —— 而这个 bug 只会在离现场很远的
        // 地方以 validation error(甚至只是画面不对)的形式冒出来。在这里直接拒绝,并
        // 点名是哪个 binding 没登记。
        for (const auto& s : info.sets)
        {
            const auto is_owned = [](const auto& b) { return engineOwnedResource(b.name) != nullptr; };

            const auto first_unowned = std::ranges::find_if_not(s.bindings, is_owned);
            if (first_unowned == s.bindings.end())
                continue; // 整个 set 都在契约里 —— 会被整体搬走
            if (std::ranges::none_of(s.bindings, is_owned))
                continue; // 整个 set 都不在契约里 —— 完全不搬

            // 报第一个缺席者就够定位:补齐契约是一次性动作,不必枚举全部。
            return renderFailure<err::shader::SetPartiallyRelocatable>(s.set, first_unowned->binding);
        }

        // 搬运表按**资源名**逐条算(relocationsFor),而不是按 shader 里的 set 号。
        //
        // set 号是 shader 自己的局部编号,不等于规范编号 —— DeferredLighting 的片元着色器
        // 就把 Light 声明在 set2(规范是 3)。按 set 号搬会把 Light 的 binding 当成 Texture
        // 搬进 BINDLESS 域,与真正的 Texture binding 撞位,而这种撞位**运行期不产生任何
        // Vulkan 错误**,只是两个不同资源读同一个描述符。spirv_patcher_test 用真实 shader
        // 抓到过。
        //
        // 按名字搬还顺带解决一个时序问题:契约与形状表都是引擎级常量,所以这一步可以在
        // feature init 期算完,不必等图编译期的 Plan。
        const auto relocs = relocationsFor(info);

        std::vector<uint32_t> words(src_bytes.size() / sizeof(uint32_t));
        std::memcpy(words.data(), src_bytes.data(), words.size() * sizeof(uint32_t));

        const auto patched = patchSpirvDescriptorPositions(std::span<uint32_t>{words}, relocs);
        if (!patched.ok)
            return renderFailure<err::shader::SpirvParseFailed>();

        // ── 门禁 2:反射必须覆盖模块里的每一个描述符 ───────────────────────────
        //
        // 门禁 1 与门禁 3 都以反射为真相源。但反射本身若只是模块的一个子集(烘焙侧的反射
        // 工具漏报,或元数据过期),按这个子集搬运就会留下没搬的残余引用 —— 而布局是按
        // 反射建的,那条残余引用的 set 在布局里根本不存在,录制期撞上「uses set #N but
        // not bound」。补丁器的第一遍会数清模块里实际存在的 (set, binding) 变量,在这里与
        // 反射对账。
        std::size_t reflected_binding_count = 0;
        for (const auto& s : info.sets)
            reflected_binding_count += s.bindings.size();

        if (patched.descriptor_count != reflected_binding_count)
            return renderFailure<err::shader::ReflectionIsSubsetOfModule>(
                static_cast<std::uint32_t>(patched.descriptor_count),
                static_cast<std::uint32_t>(reflected_binding_count)
            );

        // ── 门禁 3:该搬的都真搬动了 ──────────────────────────────────────────
        //
        // 搬运表算自**反射**,补丁改的是**缓存字节**。两者本该同源,一旦漂移(反射与字节
        // 来自不同版本),补丁器找不到目标就默默跳过,而元数据侧(relocateShaderInfoByName)
        // 仍然宣称搬过了 —— 从此 SPIR-V 与反射各说各话,Vulkan 不报错,binding 只是接错。
        std::size_t expected_moves = 0;
        for (const auto& rl : relocs)
            if (rl.to_set != rl.from_set || rl.to_binding != rl.from_binding)
                ++expected_moves;

        if (patched.relocated != expected_moves)
            return renderFailure<err::shader::ReflectionOutOfSyncWithSpirv>(
                static_cast<std::uint32_t>(expected_moves),
                static_cast<std::uint32_t>(patched.relocated)
            );

        // ── 纯恒等:就地给源记录打标记,不新增变体 ────────────────────────────
        //
        // 当没有任何资源真的移动(只用 Scene 恒等槽、或纯私有的那些 —— Grid、Tonemap、
        // blur 系),补丁输出与源字节完全相同,于是 add() 的去重必然命中源槽位,而合并标记
        // 会在那次去重里丢掉。那些管线就会永久停在未合并的老路上,继续占用 per-set 布局与
        // 旧 set 实例。所以这里直接给**源记录**打标记:恒等着色器的域模式布局与它的老路布局
        // 同形,任何共享该着色器、被动进入域模式的管线行为完全一致(它没有任何会移动的资源,
        // 两种解释在物理上是同一个)。
        if (expected_moves == 0)
        {
            records_[source.index].info.merged_domain_layout = true;
            return source;
        }

        // 元数据用**同一套**按名规则搬运,就在这一处完成 —— 拆成两个调用点,迟早有人只更新
        // 一边,而 SPIR-V 与元数据不一致时 Vulkan 不报错,binding 只是接错。
        const auto patched_bytes = std::as_bytes(std::span<const uint32_t>{words});
        const auto relocated_info = relocateShaderInfoByName(info);
        const ShaderHandle variant = add(patched_bytes, relocated_info);
        if (variant.isNull())
            return renderFailure<err::device::VulkanObjectCreationFailed>();
        return variant;
    }

    Expected<PreparedPipelineStages> ShaderResources::preparePipelineStages(std::span<const ShaderHandle> stages)
    {
        // 第一遍:把每个 stage 都切换过去。切换可能扩容 records_,所以这一遍只碰句柄,
        // 一个指针也不取。
        std::vector<ShaderHandle> switched;
        switched.reserve(stages.size());

        for (const ShaderHandle source : stages)
        {
            const ShaderObject* record = get(source);
            if (record == nullptr)
                return renderFailure<err::shader::HandleStale>();

            // 反射按值取:addMergedLayoutVariant 内部会 add(),那次调用之后 record 就可能
            // 悬垂,而它需要读原始反射。
            const lux::rdesc::ShaderInfo original_info = record->info;

            auto merged = addMergedLayoutVariant(source, original_info);
            if (!merged)
                return lux::cxx::unexpected(merged.error());
            switched.push_back(*merged);
        }

        // 第二遍:全部 add() 都已完成,此刻取模块与反射并**拷贝**下来 —— 之后无论谁再
        // add(),调用方手里的数据都不会失效。
        PreparedPipelineStages prepared;
        prepared.reserveStages(switched.size());

        for (const ShaderHandle handle : switched)
        {
            const ShaderObject* record = get(handle);
            if (record == nullptr)
                return renderFailure<err::shader::HandleStale>();
            prepared.appendStage(handle, record->module, record->info);
        }
        prepared.bindInfoPointers();
        return prepared;
    }

    const ShaderObject* ShaderResources::get(ShaderHandle handle) const noexcept
    {
        if (handle.isNull())
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
        if (handle.isNull())
            return;
        const auto idx = handle.index;
        if (idx >= static_cast<uint32_t>(records_.size()))
            return;
        if (handle.gen != gens_[idx])
            return;
        if (refcount_[idx] == 0) // already dead
            return;
        if (--refcount_[idx] != 0) // still shared by another material/instance
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
                vec.erase(
                    std::remove_if(vec.begin(), vec.end(), [idx](const SpirvCacheRec& r) { return r.slot == idx; }),
                    vec.end()
                );
                if (vec.empty())
                    spirv_cache_.erase(it);
            }
            slot_hash_[idx] = 0;
        }

        ++gens_[idx];
        free_.push_back(idx);
    }

} // namespace lux::render
