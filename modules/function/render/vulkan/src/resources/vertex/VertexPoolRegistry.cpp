/**
 * @file VertexPoolRegistry.cpp
 */

#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>


#include <lux/engine/render/gpu/VulkanContext.hpp>  // DeviceContext

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

        // 阶段 C:不再分配 per-set 实例 —— 描述符写进场景的域集,
        // 绑定也从域集取(useEngineSet)。这里只保留 layout 注册:形状表要用
        // 它建域布局,而 layout 本身归 DescriptorService 持有。
        (void)arena;

        slots_.fill(nullptr);
        initialized_ = true;
        return true;
    }

    void VertexPoolRegistry::shutdown()
    {
        if (!initialized_) return;

        // Descriptor set is pool-managed by DescriptorService — no manual
        // free call needed. Layout is owned by DescriptorService too.
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
        // 注册表满 = 这个顶点源什么都渲染不出来。没有调用方可以处置(要么这一帧
        // 已经在飞,要么调用方拿到 ~0u 之外也做不了别的),所以走自发上报。
        // 同键在一批内合并计数,不必像原先那样自己写一份幂次限流防刷屏。
        if (error_sink_ != nullptr)
            error_sink_->emit(renderError<err::frame::VertexPoolRegistryFull>(kVertexPoolMaxCount),
                              RenderErrorEvent::kNoScene, 0);
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
        w.dstArrayElement = pool_id;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo     = &info;

        // 阶段 C:域集是唯一写目标(legacy per-set 半边已删)。本集只有
        // 一个 binding,但它是数组(容量 kVertexPoolMaxCount),所以位移的是
        // dstBinding,dstArrayElement 保持 pool_id 不变。本资源只有一个集,
        // 而域集是逐 slice 的 —— 每个 slice 都要写。
        for (uint32_t s = 0; s < domain_.sliceCount(); ++s)
        {
            VkDescriptorSet domain_ds = domain_.setFor(s);
            if (domain_ds == VK_NULL_HANDLE)
                continue;
            w.dstSet     = domain_ds;
            w.dstBinding = domain_.binding(
                static_cast<uint32_t>(EVertexPoolSetBindings::VERTEX_POOLS));
            vkUpdateDescriptorSets(device_ctx_->logicalDevice(), 1, &w, 0, nullptr);
        }
    }

    Expected<void> VertexPoolRegistry::setDomainWriteTarget(std::span<const VkDescriptorSet> sets,
                                                            uint32_t binding_offset)
    {
        if (auto accepted = domain_.set(sets, binding_offset); !accepted)
            return accepted;
        // 回填已经登记过的槽位:源可能在目标设好之前就注册了(顺序无关)。
        for (std::uint32_t i = 0; i < kVertexPoolMaxCount; ++i)
            if (slots_[i])
                writeDescriptor(i, *slots_[i]);
        return {};
    }

} // namespace lux::render
