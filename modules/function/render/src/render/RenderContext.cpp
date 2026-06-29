#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/core/VulkanContext.hpp> // DeviceContext, ResourceContext
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/pipeline/ShaderPermutationCompiler.hpp>
#include <lux/engine/render/resources/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/render/resources/TextureResources.hpp> // find<TextureResources>() — builtin bindless set
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>

#include <cassert>
#include <span>
#include <stdexcept>
#include <string>

namespace lux::render
{
    RenderContext::RenderContext(ResourceContext &res_ctx, CreateInfo info)
        : resource_ctx_(res_ctx)
        , descriptor_layouts_(std::move(info.descriptor_layouts))
        , permutation_compiler_(std::move(info.permutation_compiler))
        , pipeline_mgr_(std::move(info.pipeline_mgr))
        , global_registry_(std::move(info.global_resources))
        , frames_in_flight_(info.frames_in_flight)
    {
        assert(pipeline_mgr_ && "RenderContext: pipeline_mgr must not be null");
        assert(descriptor_layouts_ && "RenderContext: descriptor_layouts must not be null");
        assert(global_registry_ && "RenderContext: global_resources must not be null");

        auto *texture_res  = global_registry_->find<TextureResources>();
        // MeshResources + MaterialResources + ShadingModelRegistry are built LAZILY
        // now (StandardMeshStack / StandardMaterial attach, or first upload — see
        // ensureGlobalMesh|MaterialResources), so they are intentionally NOT required
        // here: a pure-2D / headless server resolves no mesh arena / material stack.
        // LightResources is per-scene now (M1, Plan A) — owned by each RenderScene,
        // not resolved from the global registry here.
        if (!texture_res)
            throw std::runtime_error("RenderContext: failed to resolve builtin GPU resources.");

        vk_device_ = res_ctx.deviceContext().logicalDevice();
        descriptor_service_ = std::make_unique<DescriptorService>(
            vk_device_, res_ctx.descriptorPool());
        pipeline_layout_service_ = std::make_unique<PipelineLayoutService>(vk_device_);

        deferred_destroy_queue_.init(
            res_ctx.deviceContext().vmaAllocator(), vk_device_);
    }

    RenderContext::~RenderContext()
    {
        retire_scheduler_.flushAll();
        deferred_destroy_queue_.flushAll();
    }

    VkDevice RenderContext::device() const noexcept
    {
        return vk_device_;
    }

    DeviceContext &RenderContext::deviceContext() noexcept
    {
        return resource_ctx_.deviceContext();
    }

    VmaAllocator RenderContext::vmaAllocator() const noexcept
    {
        return resource_ctx_.deviceContext().vmaAllocator();
    }
} // namespace lux::render
