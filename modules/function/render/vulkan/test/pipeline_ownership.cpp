#include <lux/engine/gapi/vk/vk.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdio>
#include <map>
#include <string_view>
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
    bool reject_compute{}, reject_graphics{}, reject_pass{}, reject_layout{};
    unsigned computes_created{}, graphics_created{}, passes_created{};

    template <class Handle> Handle nextHandle()
    {
        return reinterpret_cast<Handle>(++next_handle);
    }

    VkResult createDescriptorLayout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* info,
        const VkAllocationCallbacks* callbacks,
        VkDescriptorSetLayout* out
    )
    {
        if (reject_layout)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return vkCreateDescriptorSetLayout(device, info, callbacks, out);
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
#define vkCreateDescriptorSetLayout createDescriptorLayout
#include "../src/gpu/memory/VmaTypes.cpp"
#define vkCreateComputePipelines createCompute
#define vkCreateGraphicsPipelines createGraphics
#define vkDestroyPipeline destroyPipeline
#define vkCreateRenderPass createPass
#define vkDestroyRenderPass destroyPass
#include "../src/gpu/VulkanContext.cpp"
#include "../src/gpu/descriptor/DescriptorService.cpp"
#include "../src/gpu/pipeline/PipelineManager.cpp"
#undef vkCreateDescriptorSetLayout
#undef vkDestroyRenderPass
#undef vkCreateRenderPass
#undef vkDestroyPipeline
#undef vkCreateGraphicsPipelines
#undef vkCreateComputePipelines
// clang-format on

namespace
{
    void testReflectedPublication(lux::render::DeviceContext& device, bool reflected_failure)
    {
        using namespace lux::render;
        auto shared_owner = GeneralDescriptorSetLayout::create(device);
        assert(shared_owner);
        auto& shared = **shared_owner;
        DescriptorService descriptors(device.logicalDevice());
        const auto maximum_sets = device.physicalDevice().properties().properties.limits.maxBoundDescriptorSets;
        PipelineLayoutService layouts(device.logicalDevice(), maximum_sets);
        PipelineManager manager(device, false);
        manager.setReflectedLayoutEnv(shared, descriptors, layouts);

        const auto shader = reinterpret_cast<VkShaderModule>(1);
        lux::rdesc::ShaderInfo info;
        info.entry_points.push_back({"main", lux::rdesc::EShaderType::COMPUTE});
        info.push_constants.push_back({0, 4});
        // Actual reflected registration must stop before creating a pipeline or publishing its record.
        auto private_info = info;
        const lux::rdesc::EDescriptorBindingInfo private_binding{
            .set = 0,
            .binding = 3,
            .type = lux::rdesc::EDescriptorType::STORAGE_BUFFER,
            .name = "private_admission_buffer"
        };
        private_info.sets.push_back({0, {private_binding}});
        {
            PipelineManager candidate(device, false);
            candidate.setReflectedLayoutEnv(shared, descriptors, layouts);
            reject_layout = true;
            const auto failed_layout = candidate.registerComputePipelineReflected(
                shader,
                private_info,
                "layout rejected"
            );
            reject_layout = false;
            assert(!failed_layout && isError<err::device::VulkanCallFailed>(failed_layout.error()));
            assert(failed_layout.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
            assert(pipelines.empty() && candidate.computeReflection({0}) == nullptr);
            assert(!descriptors.layout(0));
            const auto retry = candidate.registerComputePipelineReflected(shader, private_info, "layout retry");
            assert(retry && retry->index == 0 && candidate.computeReflection(*retry));
            assert(candidate.computeSetLayout(*retry, 0) == descriptors.layout(0));
            assert(pipelines.size() == 1);
        }
        assert(pipelines.empty());

        reject_compute = true;
        if (reflected_failure)
        {
            const auto failed = manager.registerComputePipelineReflected(shader, info, "rejected");
            std::printf("reflected failure: accepted=%d live=%zu\n", failed.has_value(), pipelines.size());
            std::fflush(stdout);
            assert(!failed && isError<err::device::VulkanCallFailed>(failed.error()));
            assert(failed.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        }
        else
        {
            const auto failed = manager.registerComputePipeline(shader, reinterpret_cast<VkPipelineLayout>(2));
            assert(!failed.valid());
        }
        assert(pipelines.empty());
        assert(manager.computeReflection({0}) == nullptr && manager.computeSetLayout({0}, 0) == VK_NULL_HANDLE);

        reject_compute = false;
        const auto accepted = manager.registerComputePipelineReflected(shader, info, "accepted");
        assert(accepted && accepted->index == 0);
        const auto* reflection = manager.computeReflection(*accepted);
        std::printf(
            "retry: index=%u reflection=%d live=%zu\n",
            accepted->index,
            reflection != nullptr,
            pipelines.size()
        );
        std::fflush(stdout);
        assert(reflection && reflection->push_constant_ranges.size() == 1);
        assert(reflection->push_constant_ranges[0].size == 4);
        assert(manager.computeSetLayout(*accepted, 0) == shared.getLayout(0));
        const auto pipeline = manager.getComputePipeline(*accepted);
        const auto layout = manager.getComputeLayout(*accepted);

        reject_compute = true;
        info.push_constants[0].size = 8;
        const auto rejected_again = manager.registerComputePipelineReflected(shader, info, "rejected-again");
        assert(!rejected_again && pipelines.size() == 1);
        assert(manager.computeReflection(*accepted)->push_constant_ranges[0].size == 4);
        assert(manager.getComputePipeline(*accepted) == pipeline && manager.getComputeLayout(*accepted) == layout);
        assert(manager.computeReflection({1}) == nullptr && manager.computeSetLayout({1}, 0) == VK_NULL_HANDLE);
        reject_compute = false;
        const auto second = manager.registerComputePipelineReflected(shader, info, "second");
        assert(second && second->index == 1 && pipelines.size() == 2);
        assert(manager.computeReflection(*second)->push_constant_ranges[0].size == 8);
        assert(manager.computeReflection(*accepted)->push_constant_ranges[0].size == 4);
        assert(manager.telemetry().compute_create_calls == 4 && manager.telemetry().compute_create_failures == 2);
    }
} // namespace

int main(int argc, char** argv)
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<PipelineManager>);
    static_assert(!std::is_move_constructible_v<PipelineManager>);
    static_assert(!std::is_copy_constructible_v<GraphicsPipelineOwner>);
    static_assert(std::is_nothrow_move_assignable_v<GraphicsPipelineOwner>);
    static_assert(!std::is_copy_constructible_v<ComputePipelineOwner>);
    static_assert(std::is_nothrow_move_constructible_v<ComputePipelineOwner>);

    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    auto device_owner = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device_owner);
    auto& device = **device_owner;
    if (argc == 2)
    {
        const std::string_view mode(argv[1]);
        assert(mode == "--rejected-reflected" || mode == "--retry-reflected");
        testReflectedPublication(device, mode == "--rejected-reflected");
        return 0;
    }
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
    testReflectedPublication(device, true);
    testReflectedPublication(device, false);
    assert(pipelines.empty() && passes.empty());
}
