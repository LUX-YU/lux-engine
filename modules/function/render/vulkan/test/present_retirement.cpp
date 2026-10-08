#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <Windows.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <type_traits>
#include <vector>
#include <vulkan/vulkan.h>

namespace
{
    VkResult queue_result{VK_SUCCESS};
    unsigned queue_waits{};
    unsigned semaphore_attempts{};
    unsigned fail_semaphore_at{};
    std::vector<char> destruction;

    VkResult VKAPI_CALL createSemaphore(
        VkDevice device,
        const VkSemaphoreCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkSemaphore* value
    )
    {
        if (++semaphore_attempts == fail_semaphore_at)
        {
            *value = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return vkCreateSemaphore(device, info, allocator, value);
    }

    VkResult VKAPI_CALL waitQueue(VkQueue queue)
    {
        ++queue_waits;
        return queue_result == VK_SUCCESS ? vkQueueWaitIdle(queue) : queue_result;
    }

    void VKAPI_CALL destroySwapchain(VkDevice device, VkSwapchainKHR value, const VkAllocationCallbacks* allocator)
    {
        destruction.push_back('C');
        vkDestroySwapchainKHR(device, value, allocator);
    }

    void VKAPI_CALL destroySemaphore(VkDevice device, VkSemaphore value, const VkAllocationCallbacks* allocator)
    {
        destruction.push_back('S');
        vkDestroySemaphore(device, value, allocator);
    }

    void VKAPI_CALL destroySurface(VkInstance instance, VkSurfaceKHR value, const VkAllocationCallbacks* allocator)
    {
        destruction.push_back('W');
        vkDestroySurfaceKHR(instance, value, allocator);
    }
} // namespace

// Observe exact native boundaries in the real three providers. All Vulkan
// allocation, submission and successful waits still execute on the real GPU.
#define vkQueueWaitIdle waitQueue
#define vkCreateSemaphore createSemaphore
#define vkDestroySwapchainKHR destroySwapchain
#define vkDestroySemaphore destroySemaphore
#define vkDestroySurfaceKHR destroySurface
#include "../src/gpu/RenderSurface.cpp"
#include "../src/targets/PresentContext.cpp"
#include "../src/targets/SwapchainProvider.cpp"
#undef vkDestroySurfaceKHR
#undef vkDestroySemaphore
#undef vkDestroySwapchainKHR
#undef vkQueueWaitIdle
#undef vkCreateSemaphore

int main()
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<PresentBacking>);
    static_assert(!std::is_move_constructible_v<PresentBacking>);
    static_assert(!std::is_copy_constructible_v<PresentRetirement>);
    static_assert(!std::is_move_constructible_v<PresentRetirement>);
    static_assert(!std::is_copy_constructible_v<PresentContext>);
    static_assert(!std::is_move_constructible_v<PresentContext>);
    const auto window = CreateWindowExW(
        0,
        L"STATIC",
        L"Present retirement",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        128,
        128,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    assert(window);
    unsigned validation_errors{};
    auto instance = InstanceContext::create(
        {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_win32_surface"},
        [&](const DebugCallbackInfo& info)
        {
            if (info.flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
            {
                ++validation_errors;
                std::fprintf(stderr, "%s\n", info.message);
            }
            return false;
        }
    );
    assert(instance);
    assert((*instance)->isInstanceExtensionEnabled(VK_EXT_DEBUG_REPORT_EXTENSION_NAME));
    auto device = DeviceContext::create(**instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device);
    auto resources = ResourceContext::create(**device);
    assert(resources);
    const VkDevice native_device = (*device)->logicalDevice();
    for (const unsigned fail_at : {1u, 3u})
    {
        PresentRetirement retirement;
        auto surface = RenderSurface::create(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window)),
            {128, 128},
            (*instance)->instance()
        );
        assert(surface);
        semaphore_attempts = 0;
        fail_semaphore_at = fail_at;
        destruction.clear();
        const auto waits_before = queue_waits;
        const auto failed = PresentContext::create(**resources, retirement, std::move(*surface), {128, 128}, true);
        assert(!failed && isError<err::device::VulkanCallFailed>(failed.error()));
        assert(failed.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(!retirement.pending() && queue_waits == waits_before);
        assert(destruction.back() == 'W');
        assert(std::count(destruction.begin(), destruction.end(), 'C') == 1);
        assert(std::count(destruction.begin(), destruction.end(), 'S') == fail_at - 1);
    }
    fail_semaphore_at = 0;
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        PresentRetirement retirement;
        auto surface = RenderSurface::create(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window)),
            {128, 128},
            (*instance)->instance()
        );
        assert(surface);
        auto context = PresentContext::create(**resources, retirement, std::move(*surface), {128, 128}, true);
        assert(context && (*context)->provider() && !retirement.pending());

        VkCommandPool pool{};
        const VkCommandPoolCreateInfo
            pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, (*device)->graphicsQueueFamilyIndex()};
        assert(vkCreateCommandPool(native_device, &pool_info, nullptr, &pool) == VK_SUCCESS);
        VkCommandBuffer command{};
        const VkCommandBufferAllocateInfo
            allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        assert(vkAllocateCommandBuffers(native_device, &allocate, &command) == VK_SUCCESS);
        VkEvent event{};
        const VkEventCreateInfo event_info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        assert(vkCreateEvent(native_device, &event_info, nullptr, &event) == VK_SUCCESS);
        VkFence fence{};
        const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        assert(vkCreateFence(native_device, &fence_info, nullptr, &fence) == VK_SUCCESS);
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
        vkCmdWaitEvents(
            command,
            1,
            &event,
            VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            nullptr,
            0,
            nullptr,
            0,
            nullptr
        );
        assert(vkEndCommandBuffer(command) == VK_SUCCESS);
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &command, 0, nullptr};
        assert(vkQueueSubmit((*device)->graphicsQueue(), 1, &submit, fence) == VK_SUCCESS);
        assert(vkGetFenceStatus(native_device, fence) == VK_NOT_READY);

        destruction.clear();
        const auto waits_before = queue_waits;
        context->reset();
        assert(retirement.pending() && destruction.empty() && queue_waits == waits_before);
        assert(vkGetFenceStatus(native_device, fence) == VK_NOT_READY);
        if (mode)
        {
            queue_result = mode == 1 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_DEVICE_LOST;
            const auto failed = retirement.settle(EPresentRetirementReason::NORMAL);
            assert(!failed && isError<err::device::VulkanCallFailed>(failed.error()));
            assert(failed.error().args[0] == encodeVkResult(queue_result));
            assert(retirement.pending() && destruction.empty());
        }
        assert(vkSetEvent(native_device, event) == VK_SUCCESS);
        assert(vkWaitForFences(native_device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
        const auto before_settle = queue_waits;
        if (mode == 1)
        {
            queue_result = VK_SUCCESS;
        }
        assert(retirement.settle(mode == 2 ? EPresentRetirementReason::DEVICE_LOST : EPresentRetirementReason::NORMAL));
        if (mode == 2)
        {
            assert(queue_waits == before_settle);
        }
        queue_result = VK_SUCCESS;
        assert(!retirement.pending() && destruction.size() > 2);
        assert(destruction.front() == 'C' && destruction.back() == 'W');
        for (std::size_t i = 1; i + 1 < destruction.size(); ++i)
        {
            assert(destruction[i] == 'S');
        }
        const auto released = destruction.size();
        assert(retirement.settle(EPresentRetirementReason::NORMAL));
        assert(destruction.size() == released);
        vkDestroyFence(native_device, fence, nullptr);
        vkDestroyEvent(native_device, event, nullptr);
        vkDestroyCommandPool(native_device, pool, nullptr);
    }
    resources->reset();
    device->reset();
    instance->reset();
    assert(!validation_errors);
    assert(DestroyWindow(window));
    std::puts("Present retirement: semantic owner ends during real GPU wait; native order, exact queue failure, "
              "retry and explicit device-loss settlement PASS");
}
