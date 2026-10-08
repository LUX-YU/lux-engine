/**
 * @file VertexPoolRegistry.cpp
 */

#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/render/resources/vertex/IVertexSource.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>

#include <lux/engine/render/gpu/VulkanContext.hpp> // DeviceContext

namespace lux::render
{
    VertexPoolRegistry::CreateResult VertexPoolRegistry::create(
        DeviceContext& device,
        std::span<const VkDescriptorSet> sets,
        std::uint32_t binding_offset,
        RenderErrorSink* error_sink
    ) noexcept
    {
        const bool is_invalid_count = sets.empty() || sets.size() > UINT32_MAX;
        const bool has_missing_set = std::ranges::any_of(sets, [](VkDescriptorSet set) { return !set; });
        const bool is_invalid_target = is_invalid_count || has_missing_set;
        if (is_invalid_target)
        {
            return renderFailure<err::descriptor::InvalidVertexPoolTarget>();
        }
        DomainWriteTarget domain;
        if (auto accepted = domain.set(sets, binding_offset); !accepted)
        {
            return lux::cxx::unexpected(accepted.error());
        }
        return std::unique_ptr<VertexPoolRegistry>(
            new VertexPoolRegistry(device.logicalDevice(), std::move(domain), error_sink)
        );
    }

    VertexPoolRegistry::VertexPoolRegistry(
        VkDevice device,
        DomainWriteTarget domain,
        RenderErrorSink* error_sink
    ) noexcept
        : device_(device), domain_(std::move(domain)), error_sink_(error_sink)
    {
    }

    std::uint32_t VertexPoolRegistry::registerSource(IVertexSource& source) noexcept
    {
        for (std::uint32_t i = 0; i < kVertexPoolMaxCount; ++i)
        {
            if (slots_[i] == nullptr)
            {
                slots_[i] = &source;
                source.setBindlessPoolId(i);
                writeDescriptor(i, source);
                return i;
            }
        }
        // 注册表满 = 这个顶点源什么都渲染不出来。没有调用方可以处置(要么这一帧
        // 已经在飞,要么调用方拿到 ~0u 之外也做不了别的),所以走自发上报。
        // 同键在一批内合并计数,不必像原先那样自己写一份幂次限流防刷屏。
        if (error_sink_ != nullptr)
            error_sink_->emit(
                renderError<err::frame::VertexPoolRegistryFull>(kVertexPoolMaxCount),
                RenderErrorEvent::kNoScene,
                0
            );
        return ~0u; // registry full
    }

    void VertexPoolRegistry::unregisterSource(std::uint32_t pool_id) noexcept
    {
        if (pool_id >= kVertexPoolMaxCount)
        {
            return;
        }

        if (slots_[pool_id])
        {
            // Tell the source it no longer has a pool id. Stale handles
            // referring to this slot will now look invalid via
            // VertexSourceHandle::valid().
            slots_[pool_id]->setBindlessPoolId(~0u);
            slots_[pool_id] = nullptr;
        }
    }

    void VertexPoolRegistry::refreshSource(std::uint32_t pool_id) noexcept
    {
        if (pool_id >= kVertexPoolMaxCount)
        {
            return;
        }
        if (slots_[pool_id])
            writeDescriptor(pool_id, *slots_[pool_id]);
    }

    bool VertexPoolRegistry::isRegistered(std::uint32_t pool_id) const noexcept
    {
        return pool_id < kVertexPoolMaxCount && slots_[pool_id] != nullptr;
    }

    void VertexPoolRegistry::writeDescriptor(std::uint32_t pool_id, IVertexSource& source) noexcept
    {
        VkBuffer buf = source.buffer();
        if (buf == VK_NULL_HANDLE)
        {
            // Source isn't ready — skip the write. Caller is expected to
            // re-register once the buffer is valid (or the source itself
            // calls back when init completes). We just bail
            // silently; the slot stays "registered" but its descriptor
            // remains the previous tenant (or VK_NULL_HANDLE if first
            // time) — UPDATE_AFTER_BIND + PARTIALLY_BOUND keep this safe
            // as long as no shader indexes into it.
            return;
        }

        VkDescriptorBufferInfo info{};
        info.buffer = buf;
        info.offset = 0;
        info.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstArrayElement = pool_id;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo = &info;

        // 阶段 C:域集是唯一写目标(legacy per-set 半边已删)。本集只有
        // 一个 binding,但它是数组(容量 kVertexPoolMaxCount),所以位移的是
        // dstBinding,dstArrayElement 保持 pool_id 不变。本资源只有一个集,
        // 而域集是逐 slice 的 —— 每个 slice 都要写。
        for (uint32_t s = 0; s < domain_.sliceCount(); ++s)
        {
            VkDescriptorSet domain_ds = domain_.setFor(s);
            w.dstSet = domain_ds;
            w.dstBinding = domain_.binding(static_cast<uint32_t>(EVertexPoolSetBindings::VERTEX_POOLS));
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
    }

} // namespace lux::render
