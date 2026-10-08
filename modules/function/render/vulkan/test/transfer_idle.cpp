#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdio>
#include <string_view>
#include <utility>

namespace
{
    VkResult next_idle = VK_SUCCESS;
    unsigned injected{};

    VkResult observedWait(VkDevice device)
    {
        // Make the hardware safe first. Inject only the API outcome consumed
        // by the real State destructor, never execute an unsafe native release.
        const auto actual = vkDeviceWaitIdle(device);
        assert(actual == VK_SUCCESS);
        const auto result = std::exchange(next_idle, VK_SUCCESS);
        if (result != VK_SUCCESS)
        {
            ++injected;
            std::puts("reached transfer idle boundary");
            std::fflush(stdout);
        }
        return result;
    }
} // namespace

#define vkDeviceWaitIdle observedWait
#include "../src/gpu/VulkanContext.cpp"
#undef vkDeviceWaitIdle

int main(int argc, char** argv)
{
    using namespace lux::render;
    const std::string_view mode = argc == 2 ? argv[1] : "success";
    assert(mode == "success" || mode == "lost" || mode == "failed");
    auto instance = InstanceContext::create({});
    assert(instance);
    auto device = DeviceContext::create(**instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device);
    GpuTransferPipeline::Config config;
    config.device_ctx = device->get();
    config.batch_slot_count = 2;
    config.queue_capacity = 4;
    config.result_capacity = 4;
    auto pipeline = GpuTransferPipeline::create(config);
    assert(pipeline);
    next_idle = mode == "lost" ? VK_ERROR_DEVICE_LOST : mode == "failed" ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
    pipeline->reset();
    assert(mode != "failed"); // Rejected wait cannot authorize ordinary release.
    assert(injected == (mode == "success" ? 0u : 1u));
    std::puts("PASS transfer native idle retirement boundary");
}
