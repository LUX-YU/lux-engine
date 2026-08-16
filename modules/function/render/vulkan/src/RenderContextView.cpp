#include <lux/engine/render/RenderContextView.hpp>

// The subject this facade forwards to + the concrete services it exposes.
// RenderContextView HOLDS a RenderContext& (composition); it does NOT inherit from /
// is not a base of RenderContext — the dependency flows feature → facade → subject.
#include <lux/engine/render/gpu/RenderContext.hpp>                    // RenderContext (held subject; also pulls DeferredDestroyQueue)
#include <lux/engine/render/resources/ShaderResources.hpp>          // ShaderResources::add/get
#include <lux/engine/render/gpu/ShaderObject.hpp>                  // ShaderObject::module/.info
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>// getLayout(slot)
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>           // registerGraphicsTemplate/registerComputePipeline
#include <lux/engine/render/gpu/pipeline/StandardPipelineLayoutBuilder.hpp> // buildStandardGraphicsPipelineLayout
#include <lux/engine/function/render/client/core/Errors.hpp>                        // Expected / renderFailure

namespace lux::render
{
    // ── Vulkan infrastructure ───────────────────────────────────────────
    VkDevice RenderContextView::device() const noexcept
    {
        return ctx_->device();
    }

    VmaAllocator RenderContextView::vmaAllocator() const noexcept
    {
        return ctx_->vmaAllocator();
    }

    uint32_t RenderContextView::framesInFlight() const noexcept
    {
        return ctx_->framesInFlight();
    }

    // ── Global resource registry ────────────────────────────────────────
    ResourceRegistry& RenderContextView::globalRegistry() noexcept
    {
        return ctx_->globalRegistry();
    }

    const ResourceRegistry& RenderContextView::globalRegistry() const noexcept
    {
        return ctx_->globalRegistry();
    }

    // ── Shaders ─────────────────────────────────────────────────────────
    ShaderHandle RenderContextView::createShaderModule(std::span<const std::byte> spirv, const lux::rdesc::ShaderInfo& info)
    {
        return ctx_->globalRegistry().must<ShaderResources>().add(spirv, info);
    }

    VkShaderModule RenderContextView::shaderModule(ShaderHandle handle) const
    {
        const auto* obj = ctx_->globalRegistry().must<ShaderResources>().get(handle);
        return obj ? obj->module : VkShaderModule{};
    }

    const lux::rdesc::ShaderInfo* RenderContextView::shaderInfo(ShaderHandle handle) const
    {
        const auto* obj = ctx_->globalRegistry().must<ShaderResources>().get(handle);
        return obj ? &obj->info : nullptr;
    }

    // ── Descriptor-set layouts ──────────────────────────────────────────
    VkDescriptorSetLayout RenderContextView::engineSetLayout(EDescriptorSetSlot slot)
    {
        return ctx_->descriptorLayouts().getLayout(slot);
    }

    // ── Pipeline layout + pipeline registration (Expected unwrapped here) ──
    VkPipelineLayout RenderContextView::buildStandardGraphicsLayout(
        uint32_t                                       descriptor_set_count,
        std::span<const lux::rdesc::ShaderInfo* const> shader_infos,
        std::string_view                               debug_name)
    {
        return buildStandardGraphicsPipelineLayout(
            *ctx_, descriptor_set_count, shader_infos, debug_name).value();
    }

    GraphicsPipelineHandle RenderContextView::registerGraphics(
        const GraphicsPipelineTemplate&                  description,
        std::span<const lux::rdesc::ShaderInfo* const> shader_infos)
    {
        return ctx_->pipelineManager().registerGraphicsTemplate(description, shader_infos).value();
    }

    ComputePipelineHandle RenderContextView::registerCompute(VkShaderModule shader, VkPipelineLayout layout)
    {
        return ctx_->pipelineManager().registerComputePipeline(shader, layout);
    }

    // ── Feature-owned GPU-resource lifecycle plumbing ───────────────────
    DeferredDestroyQueue& RenderContextView::deferredDestroyQueue() noexcept
    {
        return ctx_->deferredDestroyQueue();
    }

    FrameRetireScheduler& RenderContextView::retireScheduler() noexcept
    {
        return ctx_->retireScheduler();
    }

    // ── Retire feature-owned GPU resources (forward to the queue) ────────
    void RenderContextView::retireBuffer(VkBuffer buffer, VmaAllocation allocation)
    {
        ctx_->deferredDestroyQueue().retireBuffer(buffer, allocation);
    }

    void RenderContextView::retireImage(VkImage image, VmaAllocation allocation)
    {
        ctx_->deferredDestroyQueue().retireImage(image, allocation);
    }

    void RenderContextView::retireImageView(VkImageView view)
    {
        ctx_->deferredDestroyQueue().retireImageView(view);
    }

    void RenderContextView::retireSampler(VkSampler sampler)
    {
        ctx_->deferredDestroyQueue().retireSampler(sampler);
    }

    // ── External-memory interop (forward to the subject) ─────────────────
    bool RenderContextView::supportsExternalMemory() noexcept
    {
        return ctx_->supportsExternalMemory();
    }

    void RenderContextView::deviceUUID(uint8_t out[16]) noexcept
    {
        ctx_->deviceUUID(out);
    }

    uint32_t RenderContextView::findMemoryTypeIndex(uint32_t type_filter, uint32_t property_flags) noexcept
    {
        return ctx_->findMemoryTypeIndex(type_filter, property_flags);
    }

    Expected<ExportableBuffer> RenderContextView::createExportableBuffer(
        uint64_t size,
        uint32_t usage_flags
    )
    {
        return ctx_->createExportableBuffer(size, usage_flags);
    }

    Expected<ExportableTimelineSemaphore>
    RenderContextView::createExportableTimelineSemaphore()
    {
        return ctx_->createExportableTimelineSemaphore();
    }

    void RenderContextView::retireRawBuffer(VkBuffer buffer)
    {
        ctx_->deferredDestroyQueue().retireRawBuffer(buffer);
    }

    void RenderContextView::retireDeviceMemory(VkDeviceMemory memory)
    {
        ctx_->deferredDestroyQueue().retireDeviceMemory(memory);
    }

    void RenderContextView::retireSemaphore(VkSemaphore semaphore)
    {
        ctx_->deferredDestroyQueue().retireSemaphore(semaphore);
    }

} // namespace lux::render
