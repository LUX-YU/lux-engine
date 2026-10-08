#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>

#include <cassert>
#include <map>
#include <type_traits>

namespace
{
    struct PipelineOrigin
    {
        VkDevice device{};
        const VkAllocationCallbacks* allocator{};
        VkRenderPass render_pass{};
    };

    std::map<VkPipeline, PipelineOrigin> pipelines;
    std::map<VkRenderPass, VkDevice> passes;
    std::uintptr_t next_handle{100};
    bool reject_compute{}, reject_graphics{}, reject_pass{};
    unsigned computes_created{}, graphics_created{}, passes_created{};

    template <class Handle> Handle nextHandle()
    {
        return reinterpret_cast<Handle>(++next_handle);
    }

    VkResult createCompute(
        VkDevice device,
        VkPipelineCache cache,
        uint32_t count,
        const VkComputePipelineCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkPipeline* output
    )
    {
        assert(cache == VK_NULL_HANDLE && count == 1 && info->layout != VK_NULL_HANDLE);
        *output = VK_NULL_HANDLE;
        if (reject_compute)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        *output = nextHandle<VkPipeline>();
        assert(pipelines.emplace(*output, PipelineOrigin{device, allocator, {}}).second);
        ++computes_created;
        return VK_SUCCESS;
    }

    VkResult createGraphics(
        VkDevice device,
        VkPipelineCache cache,
        uint32_t count,
        const VkGraphicsPipelineCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkPipeline* output
    )
    {
        assert(cache == VK_NULL_HANDLE && count == 1 && info->layout != VK_NULL_HANDLE);
        assert(info->renderPass == VK_NULL_HANDLE || passes.contains(info->renderPass));
        *output = VK_NULL_HANDLE;
        if (reject_graphics)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        *output = nextHandle<VkPipeline>();
        assert(pipelines.emplace(*output, PipelineOrigin{device, allocator, info->renderPass}).second);
        ++graphics_created;
        return VK_SUCCESS;
    }

    void destroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator)
    {
        const auto found = pipelines.find(pipeline);
        assert(found != pipelines.end());
        assert(found->second.device == device && found->second.allocator == allocator);
        const auto pass = found->second.render_pass;
        assert(pass == VK_NULL_HANDLE || passes.contains(pass));
        pipelines.erase(found);
    }

    VkResult createPass(VkDevice device, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass* out)
    {
        *out = VK_NULL_HANDLE;
        if (reject_pass)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        *out = nextHandle<VkRenderPass>();
        assert(passes.emplace(*out, device).second);
        ++passes_created;
        return VK_SUCCESS;
    }

    void destroyPass(VkDevice device, VkRenderPass pass, const VkAllocationCallbacks*)
    {
        const auto found = passes.find(pass);
        assert(found != passes.end() && found->second == device);
        for (const auto& [pipeline, origin] : pipelines)
        {
            assert(origin.render_pass != pass);
        }
        passes.erase(found);
    }
} // namespace

// Real cache/key/telemetry algorithms, with deterministic native create/destroy endpoints.
// Actual pipeline rendering is covered by the separate framework GPU regression.
// clang-format off
#include "../src/gpu/memory/VmaTypes.cpp"
#include "../src/gpu/VulkanContext.cpp"
#define vkCreateComputePipelines createCompute
#define vkCreateGraphicsPipelines createGraphics
#define vkDestroyPipeline destroyPipeline
#define vkCreateRenderPass createPass
#define vkDestroyRenderPass destroyPass
#include "../src/gpu/pipeline/PipelineManager.cpp"
#undef vkDestroyRenderPass
#undef vkCreateRenderPass
#undef vkDestroyPipeline
#undef vkCreateGraphicsPipelines
#undef vkCreateComputePipelines
// clang-format on

int main()
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<PipelineManager>);
    static_assert(!std::is_move_constructible_v<PipelineManager>);
    static_assert(!std::is_copy_constructible_v<GraphicsPipelineOwner>);
    static_assert(std::is_nothrow_move_assignable_v<GraphicsPipelineOwner>);
    static_assert(!std::is_copy_constructible_v<ComputePipelineOwner>);
    static_assert(std::is_nothrow_move_constructible_v<ComputePipelineOwner>);

    InstanceContext instance({});
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    const auto shader = reinterpret_cast<VkShaderModule>(1);
    const auto layout = reinterpret_cast<VkPipelineLayout>(2);

    {
        const VkAllocationCallbacks first_callbacks{}, second_callbacks{};
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.layout = layout;
        auto first = ComputePipelineOwner::create(device.logicalDevice(), info, &first_callbacks);
        auto second = ComputePipelineOwner::create(device.logicalDevice(), info, &second_callbacks);
        assert(first && second && pipelines.size() == 2);
        *first = std::move(*second);
        assert(!*second && pipelines.size() == 1);
        ComputePipelineOwner moved(std::move(*first));
        assert(!*first && moved);
    }
    assert(pipelines.empty());

    for (const bool dynamic : {false, true})
    {
        {
            PipelineManager manager(device, dynamic);
            reject_compute = true;
            const auto failed = manager.registerComputePipeline(shader, layout);
            assert(!failed.valid() && pipelines.empty());
            reject_compute = false;
            const auto compute = manager.registerComputePipeline(shader, layout);
            assert(compute.valid() && manager.getComputePipeline(compute) != VK_NULL_HANDLE);
            assert(manager.getComputeLayout(compute) == layout);
            assert(manager.telemetry().compute_create_calls == 2 && manager.telemetry().compute_create_failures == 1);

            const RenderPassKey key{};
            if (!dynamic)
            {
                reject_pass = true;
                assert(manager.getOrCreateRenderPass(key) == VK_NULL_HANDLE && passes.empty());
                reject_pass = false;
                const auto pass = manager.getOrCreateRenderPass(key);
                const auto count = passes_created;
                assert(pass != VK_NULL_HANDLE && manager.getOrCreateRenderPass(key) == pass);
                assert(passes_created == count);
            }

            GraphicsPipelineTemplate description{};
            description.pipeline_layout = layout;
            description.vertex_shader = shader;
            description.fragment_shader = shader;
            const auto graphics = manager.registerGraphicsTemplate(description);
            assert(graphics);
            reject_graphics = true;
            assert(manager.getOrCreatePipeline(*graphics, key, 0) == VK_NULL_HANDLE);
            reject_graphics = false;
            const auto pipeline = manager.getOrCreatePipeline(*graphics, key, 0);
            assert(pipeline != VK_NULL_HANDLE);
            const auto count = graphics_created;
            assert(manager.getOrCreatePipeline(*graphics, key, 0) == pipeline && graphics_created == count);
            assert(manager.telemetry().graphics_create_failures == 1 && manager.telemetry().graphics_cache_hits == 1);
            assert(pipelines.size() == 2);
        }
        assert(pipelines.empty() && passes.empty());
    }
}
