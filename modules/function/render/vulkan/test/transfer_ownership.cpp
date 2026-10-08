#include <lux/engine/gapi/vk/vk.hpp>

#include <vulkan/vulkan.h>

#include <cassert>
#include <cstdio>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace
{
#if defined(LUX_TRANSFER_THREAD_FAILURE_TEST)
    bool reject_thread{};

    template <class F> std::thread makeTransferThread(F&& function)
    {
        if (reject_thread)
        {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        return std::thread(std::forward<F>(function));
    }
#endif

    std::unordered_map<VkSemaphore, VkDevice> semaphores;
    std::unordered_map<VkCommandPool, VkDevice> pools;
    unsigned attempts{}, fail_at{};

    VkResult createSemaphore(
        VkDevice device,
        const VkSemaphoreCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkSemaphore* output
    )
    {
        ++attempts;
        *output = VK_NULL_HANDLE;
        if (attempts == fail_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateSemaphore(device, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            assert(semaphores.emplace(*output, device).second);
        }
        return result;
    }

    void destroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* allocator)
    {
        const auto it = semaphores.find(semaphore);
        assert(it != semaphores.end() && it->second == device);
        semaphores.erase(it);
        vkDestroySemaphore(device, semaphore, allocator);
    }

    VkResult createPool(
        VkDevice device,
        const VkCommandPoolCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkCommandPool* output
    )
    {
        ++attempts;
        *output = VK_NULL_HANDLE;
        if (attempts == fail_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateCommandPool(device, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            assert(pools.emplace(*output, device).second);
        }
        return result;
    }

    void destroyPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator)
    {
        const auto it = pools.find(pool);
        assert(it != pools.end() && it->second == device);
        pools.erase(it);
        vkDestroyCommandPool(device, pool, allocator);
    }
} // namespace

// Run the actual worker and device path. Only creation faults and destruction accounting are injected.
// clang-format off
#define vkCreateSemaphore createSemaphore
#define vkDestroySemaphore destroySemaphore
#define vkCreateCommandPool createPool
#define vkDestroyCommandPool destroyPool
#include <lux/engine/render/gpu/VulkanContext.hpp>
#if defined(LUX_TRANSFER_THREAD_FAILURE_TEST)
#include "GpuTransferPipelineThreadFailure.cpp"
#else
#include "../src/resources/lifecycle/GpuTransferPipeline.cpp"
#endif
#undef vkDestroyCommandPool
#undef vkCreateCommandPool
#undef vkDestroySemaphore
#undef vkCreateSemaphore
// clang-format on

int main()
{
    using namespace lux::render;
    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    auto device_owner = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device_owner);
    auto& device = **device_owner;
    static_assert(noexcept(GpuTransferPipeline::create(std::declval<const GpuTransferPipeline::Config&>())));
    static_assert(!std::is_default_constructible_v<GpuTransferPipeline>);
    static_assert(!std::is_move_constructible_v<GpuTransferPipeline>);
    static_assert(!std::is_copy_constructible_v<GpuTransferPipeline>);
    GpuTransferPipeline::Config config;
    config.device_ctx = &device;
    config.batch_slot_count = 3;
    config.queue_capacity = 4;
    config.result_capacity = 8;
    for (unsigned boundary = 1; boundary <= config.batch_slot_count + 1; ++boundary)
    {
        attempts = 0;
        fail_at = boundary;
        const auto candidate = GpuTransferPipeline::create(config);
        assert(!candidate && isError<err::device::VulkanCallFailed>(candidate.error()));
        assert(attempts == boundary && semaphores.empty() && pools.empty());
    }
    fail_at = 0;
#if defined(LUX_TRANSFER_THREAD_FAILURE_TEST)
    reject_thread = true;
    try
    {
        const auto failure = GpuTransferPipeline::create(config);
        assert(!failure && isError<err::upload::WorkerStartFailed>(failure.error()));
        assert(failure.error().args[0] == static_cast<std::uint32_t>(std::errc::resource_unavailable_try_again));
        assert(failure.error().args[1] == 0u);
        assert(semaphores.empty() && pools.empty());
        std::puts("PASS thread acquisition failure returned after complete native cleanup");
    }
    catch (const std::system_error& error)
    {
        std::fprintf(
            stderr,
            "FAIL thread acquisition escaped factory: code=%d semaphores=%zu pools=%zu\n",
            error.code().value(),
            semaphores.size(),
            pools.size()
        );
        return 1;
    }
    reject_thread = false;
#endif
    for (unsigned iteration = 0; iteration < 128; ++iteration)
    {
        {
            auto pipeline = GpuTransferPipeline::create(config);
            assert(pipeline && (*pipeline)->timelineSemaphore() != VK_NULL_HANDLE);
            assert(semaphores.size() == 1 && pools.size() == config.batch_slot_count);
            if (iteration == 0)
            {
                (*pipeline)->stopAndDrain();
                (*pipeline)->stopAndDrain();
                assert(!(*pipeline)->submitMeshTransfer(MeshTransferTask{}));
                TransferCompletion completion{};
                assert((*pipeline)->drainResults(&completion, 1) == 0);
                assert(semaphores.size() == 1 && pools.size() == config.batch_slot_count);
            }
        }
        assert(semaphores.empty() && pools.empty());
    }
}
