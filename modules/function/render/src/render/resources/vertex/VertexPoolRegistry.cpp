/**
 * @file VertexPoolRegistry.cpp
 */

#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/descriptor/SceneDescriptorArena.hpp>

#include <iostream>

#include <lux/engine/render/core/VulkanContext.hpp>  // DeviceContext

namespace lux::render
{
    VertexPoolRegistry::~VertexPoolRegistry()
    {
        if (initialized_) shutdown();
    }

    bool VertexPoolRegistry::init(DeviceContext&        device_ctx,
                                  DescriptorService&    descriptor_svc,
                                  SceneDescriptorArena& arena)
    {
        if (initialized_) return true;

        device_ctx_     = &device_ctx;
        descriptor_svc_ = &descriptor_svc;

        // Register the layout with DescriptorService. Must mirror the layout
        // built by GeneralDescriptorSetLayout for SET 7 so pipelines built
        // with either source produce compatible pipeline layouts.
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = static_cast<uint32_t>(EVertexPoolSetBindings::VERTEX_POOLS);
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = kVertexPoolMaxCount;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT
                                | VK_SHADER_STAGE_FRAGMENT_BIT
                                | VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorBindingFlags bf =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

        DescriptorLayoutDesc desc{};
        desc.bindings      = std::span<const VkDescriptorSetLayoutBinding>(&binding, 1);
        desc.binding_flags = std::span<const VkDescriptorBindingFlags>(&bf, 1);
        desc.flags         = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        desc.debug_name    = "VertexPoolRegistry";

        layout_id_ = descriptor_svc_->registerLayout(desc);
        if (layout_id_ == kInvalidDescriptorLayoutId) {
            device_ctx_     = nullptr;
            descriptor_svc_ = nullptr;
            return false;
        }

        ds_ = arena.allocate(descriptor_svc_->layout(layout_id_));
        if (ds_ == VK_NULL_HANDLE) {
            // DescriptorService keeps the layout, but the set allocation
            // failed. Roll back our state — caller can retry later.
            layout_id_      = kInvalidDescriptorLayoutId;
            device_ctx_     = nullptr;
            descriptor_svc_ = nullptr;
            return false;
        }

        slots_.fill(nullptr);
        initialized_ = true;
        return true;
    }

    void VertexPoolRegistry::shutdown()
    {
        if (!initialized_) return;

        // Descriptor set is pool-managed by DescriptorService — no manual
        // free call needed. Layout is owned by DescriptorService too.
        ds_             = VK_NULL_HANDLE;
        layout_id_      = kInvalidDescriptorLayoutId;
        device_ctx_     = nullptr;
        descriptor_svc_ = nullptr;
        slots_.fill(nullptr);
        initialized_    = false;
    }

    std::uint32_t VertexPoolRegistry::registerSource(IVertexSource& source)
    {
        if (!initialized_) return ~0u;

        for (std::uint32_t i = 0; i < kVertexPoolMaxCount; ++i) {
            if (slots_[i] == nullptr) {
                slots_[i] = &source;
                source.setBindlessPoolId(i);
                writeDescriptor(i, source);
                return i;
            }
        }
        {
            // Loud-fail: registry full → this source renders nothing. Throttled
            // (power-of-2) so a persistently-over-budget scene doesn't spam.
            static std::uint32_t warn_n = 0;
            if ((++warn_n & (warn_n - 1)) == 0)
                std::cerr << "[VertexPoolRegistry] registry full (kVertexPoolMaxCount="
                          << kVertexPoolMaxCount << "); vertex source rejected (x"
                          << warn_n << ")\n";
        }
        return ~0u;  // registry full
    }

    void VertexPoolRegistry::unregisterSource(std::uint32_t pool_id)
    {
        if (!initialized_ || pool_id >= kVertexPoolMaxCount) return;

        if (slots_[pool_id]) {
            // Tell the source it no longer has a pool id. Stale handles
            // referring to this slot will now look invalid via
            // VertexSourceHandle::valid().
            slots_[pool_id]->setBindlessPoolId(~0u);
            slots_[pool_id] = nullptr;
        }
    }

    void VertexPoolRegistry::refreshSource(std::uint32_t pool_id)
    {
        if (!initialized_ || pool_id >= kVertexPoolMaxCount) return;
        if (slots_[pool_id])
            writeDescriptor(pool_id, *slots_[pool_id]);
    }

    bool VertexPoolRegistry::isRegistered(std::uint32_t pool_id) const noexcept
    {
        return initialized_
            && pool_id < kVertexPoolMaxCount
            && slots_[pool_id] != nullptr;
    }

    VkDescriptorSetLayout VertexPoolRegistry::descriptorSetLayout() const noexcept
    {
        return descriptor_svc_
            ? descriptor_svc_->layout(layout_id_)
            : VK_NULL_HANDLE;
    }

    void VertexPoolRegistry::writeDescriptor(std::uint32_t pool_id, IVertexSource& source)
    {
        VkBuffer buf = source.buffer();
        if (buf == VK_NULL_HANDLE) {
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
        info.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet          = ds_;
        w.dstBinding      = static_cast<uint32_t>(EVertexPoolSetBindings::VERTEX_POOLS);
        w.dstArrayElement = pool_id;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo     = &info;

        vkUpdateDescriptorSets(device_ctx_->logicalDevice(), 1, &w, 0, nullptr);
    }

} // namespace lux::render
