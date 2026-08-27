// ============================================================================
// FrameDriver construction transaction — CPU-only failure-injection test.
//
// The test calls the same local RAII candidate used by FrameDriver::create(),
// but supplies function pointers backed by deterministic fake Vulkan handles.
// This proves rollback order and the no-half-object publication boundary without
// opening a device or replacing global Vulkan entry points.
// ============================================================================

#include <lux/engine/render/renderer/FrameDriver.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/targets/PresentContext.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

using namespace lux::render;

// The old failure-hiding constructors are not compatibility API. If any of
// them returns, this test must fail at compile time before runtime coverage can
// accidentally exercise the wrong path.
static_assert(!std::is_constructible_v<FrameDriver, ResourceContext&, std::uint32_t>);
static_assert(!std::is_constructible_v<lux::gapi::vk::Fence, VkDevice>);
static_assert(!std::is_constructible_v<lux::gapi::vk::CommandBuffer, VkDevice, VkCommandPool>);
static_assert(std::is_move_constructible_v<lux::gapi::vk::Fence>);
static_assert(!std::is_move_assignable_v<lux::gapi::vk::Fence>);
static_assert(std::is_move_constructible_v<lux::gapi::vk::CommandBuffer>);
static_assert(!std::is_move_assignable_v<lux::gapi::vk::CommandBuffer>);
static_assert(!std::is_constructible_v<lux::gapi::vk::Semaphore, VkDevice>);
static_assert(std::is_move_constructible_v<lux::gapi::vk::Semaphore>);
static_assert(std::is_move_assignable_v<lux::gapi::vk::Semaphore>);
static_assert(!std::is_constructible_v<RenderContext, ResourceContext&, RenderContext::CreateInfo>);
static_assert(
    std::is_same_v<
        decltype(RenderContext::create(std::declval<ResourceContext&>(), std::declval<RenderContext::CreateInfo>())),
        Expected<std::shared_ptr<RenderContext>>>);
static_assert(
    std::is_same_v<decltype(std::declval<RenderContext&>().createExportableBuffer(1, 0)), Expected<ExportableBuffer>>);
static_assert(std::is_same_v<decltype(std::declval<lux::gapi::vk::LogicalDevice&>().waitIdle()), VkResult>);
static_assert(
    std::is_same_v<decltype(std::declval<lux::gapi::vk::Fence&>().reset(std::declval<VkDevice>())), VkResult>);
static_assert(std::is_same_v<decltype(std::declval<lux::gapi::vk::CommandBuffer&>().reset()), VkResult>);
static_assert(std::is_same_v<decltype(std::declval<lux::gapi::vk::CommandBuffer&>().begin()), VkResult>);
static_assert(std::is_same_v<decltype(std::declval<lux::gapi::vk::CommandBuffer&>().end()), VkResult>);

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{
    template <typename Handle> [[nodiscard]] Handle fakeHandle(std::uintptr_t value) noexcept
    {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<Handle>(value);
        else
            return static_cast<Handle>(value);
    }

    template <typename Handle> [[nodiscard]] std::uintptr_t handleValue(Handle handle) noexcept
    {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<std::uintptr_t>(handle);
        else
            return static_cast<std::uintptr_t>(handle);
    }

    enum class ECleanup : std::uint8_t
    {
        FREE_COMMAND_BUFFER,
        DESTROY_FENCE,
        DESTROY_SEMAPHORE,
    };

    enum class EFrameCall : std::uint8_t
    {
        WAIT_FENCE,
        RESET_FENCE,
        RESET_COMMAND_BUFFER,
        BEGIN_COMMAND_BUFFER,
        END_COMMAND_BUFFER,
    };

    struct CleanupCall final
    {
        ECleanup kind{};
        std::uintptr_t handle{0};
    };

    struct FakeVulkan final
    {
        static constexpr std::uint32_t kNever = (std::numeric_limits<std::uint32_t>::max)();

        std::uint32_t fail_fence_at{kNever};
        std::uint32_t fail_command_buffer_at{kNever};
        std::uint32_t fail_semaphore_at{kNever};
        std::uint32_t null_semaphore_at{kNever};
        std::uint32_t fence_calls{0};
        std::uint32_t command_buffer_calls{0};
        std::uint32_t semaphore_calls{0};
        std::array<CleanupCall, 4 * kMaxFramesInFlight + 4> cleanup{};
        std::uint32_t cleanup_count{0};
        VkResult wait_result{VK_SUCCESS};
        VkResult reset_fence_result{VK_SUCCESS};
        VkResult reset_command_buffer_result{VK_SUCCESS};
        VkResult begin_command_buffer_result{VK_SUCCESS};
        VkResult end_command_buffer_result{VK_SUCCESS};
        VkResult queue_wait_result{VK_SUCCESS};
        std::uint32_t queue_wait_calls{0};
        std::array<EFrameCall, 4> frame_calls{};
        std::uint32_t frame_call_count{0};
    };

    FakeVulkan g_fake{};

    constexpr std::uintptr_t kFenceBase = 0x1000;
    constexpr std::uintptr_t kCommandBufferBase = 0x2000;
    constexpr std::uintptr_t kSemaphoreBase = 0x3000;

    VKAPI_ATTR VkResult VKAPI_CALL
    fakeCreateFence(VkDevice, const VkFenceCreateInfo* info, const VkAllocationCallbacks*, VkFence* out_fence)
    {
        const std::uint32_t call = g_fake.fence_calls++;
        if (call == g_fake.fail_fence_at)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;

        if (info == nullptr || info->flags != VK_FENCE_CREATE_SIGNALED_BIT)
            return VK_ERROR_INITIALIZATION_FAILED;

        *out_fence = fakeHandle<VkFence>(kFenceBase + call);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL fakeDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*)
    {
        g_fake.cleanup[g_fake.cleanup_count++] = CleanupCall{
            ECleanup::DESTROY_FENCE,
            handleValue(fence),
        };
    }

    VKAPI_ATTR VkResult VKAPI_CALL
    fakeAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out_command_buffer)
    {
        const std::uint32_t call = g_fake.command_buffer_calls++;
        if (call == g_fake.fail_command_buffer_at)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;

        if (info == nullptr || info->commandBufferCount != 1 || info->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        *out_command_buffer = fakeHandle<VkCommandBuffer>(kCommandBufferBase + call);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL
    fakeFreeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t count, const VkCommandBuffer* command_buffers)
    {
        if (count != 1)
            return;
        g_fake.cleanup[g_fake.cleanup_count++] = CleanupCall{
            ECleanup::FREE_COMMAND_BUFFER,
            handleValue(command_buffers[0]),
        };
    }

    VKAPI_ATTR VkResult VKAPI_CALL fakeCreateSemaphore(
        VkDevice,
        const VkSemaphoreCreateInfo* info,
        const VkAllocationCallbacks*,
        VkSemaphore* out_semaphore
    )
    {
        const std::uint32_t call = g_fake.semaphore_calls++;
        if (call == g_fake.fail_semaphore_at)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (info == nullptr || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (call == g_fake.null_semaphore_at)
        {
            *out_semaphore = VK_NULL_HANDLE;
            return VK_SUCCESS;
        }
        *out_semaphore = fakeHandle<VkSemaphore>(kSemaphoreBase + call);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL fakeDestroySemaphore(VkDevice, VkSemaphore semaphore, const VkAllocationCallbacks*)
    {
        g_fake.cleanup[g_fake.cleanup_count++] = CleanupCall{
            ECleanup::DESTROY_SEMAPHORE,
            handleValue(semaphore),
        };
    }

    VkResult fakeWaitFrameFence(lux::gapi::vk::Fence&, VkDevice) noexcept
    {
        g_fake.frame_calls[g_fake.frame_call_count++] = EFrameCall::WAIT_FENCE;
        return g_fake.wait_result;
    }

    VkResult fakeResetFrameFence(lux::gapi::vk::Fence&, VkDevice) noexcept
    {
        g_fake.frame_calls[g_fake.frame_call_count++] = EFrameCall::RESET_FENCE;
        return g_fake.reset_fence_result;
    }

    VkResult fakeResetFrameCommandBuffer(lux::gapi::vk::CommandBuffer&) noexcept
    {
        g_fake.frame_calls[g_fake.frame_call_count++] = EFrameCall::RESET_COMMAND_BUFFER;
        return g_fake.reset_command_buffer_result;
    }

    VkResult fakeBeginFrameCommandBuffer(lux::gapi::vk::CommandBuffer&, VkCommandBufferUsageFlags) noexcept
    {
        g_fake.frame_calls[g_fake.frame_call_count++] = EFrameCall::BEGIN_COMMAND_BUFFER;
        return g_fake.begin_command_buffer_result;
    }

    VkResult fakeEndFrameCommandBuffer(lux::gapi::vk::CommandBuffer&) noexcept
    {
        g_fake.frame_calls[g_fake.frame_call_count++] = EFrameCall::END_COMMAND_BUFFER;
        return g_fake.end_command_buffer_result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL fakeQueueWaitIdle(VkQueue)
    {
        ++g_fake.queue_wait_calls;
        return g_fake.queue_wait_result;
    }

    [[nodiscard]] detail::FrameDriverCreateOps fakeOps() noexcept
    {
        return detail::FrameDriverCreateOps{
            &fakeCreateFence,
            &fakeDestroyFence,
            &fakeAllocateCommandBuffers,
            &fakeFreeCommandBuffers,
        };
    }

    [[nodiscard]] detail::FrameDriverRuntimeOps fakeRuntimeOps() noexcept
    {
        return detail::FrameDriverRuntimeOps{
            .wait_fence = &fakeWaitFrameFence,
            .reset_fence = &fakeResetFrameFence,
            .reset_command_buffer = &fakeResetFrameCommandBuffer,
            .begin_command_buffer = &fakeBeginFrameCommandBuffer,
            .end_command_buffer = &fakeEndFrameCommandBuffer,
        };
    }

    [[nodiscard]] detail::PresentSemaphoreCreateOps fakeSemaphoreOps() noexcept
    {
        return detail::PresentSemaphoreCreateOps{
            &fakeCreateSemaphore,
            &fakeDestroySemaphore,
        };
    }

    [[nodiscard]] VkDevice fakeDevice() noexcept
    {
        return fakeHandle<VkDevice>(0xD000);
    }

    [[nodiscard]] VkCommandPool fakeCommandPool() noexcept
    {
        return fakeHandle<VkCommandPool>(0xC000);
    }

    bool invalidFrameCountFailsBeforeVulkan()
    {
        g_fake = FakeVulkan{};
        auto& fake = g_fake;

        auto zero = detail::FrameDriverCreateCandidate::create(fakeDevice(), fakeCommandPool(), 0, fakeOps());
        CHECK(!zero);
        CHECK(isError<err::device::InvalidFramesInFlight>(zero.error()));
        CHECK(zero.error().args[0] == 0);
        CHECK(zero.error().args[1] == kMaxFramesInFlight);

        auto too_many = detail::FrameDriverCreateCandidate::create(
            fakeDevice(),
            fakeCommandPool(),
            kMaxFramesInFlight + 1,
            fakeOps()
        );
        CHECK(!too_many);
        CHECK(isError<err::device::InvalidFramesInFlight>(too_many.error()));
        CHECK(too_many.error().args[0] == kMaxFramesInFlight + 1);
        CHECK(fake.fence_calls == 0);
        CHECK(fake.command_buffer_calls == 0);
        CHECK(fake.cleanup_count == 0);
        return true;
    }

    bool invalidCreateOpsFailBeforeCall()
    {
        g_fake = FakeVulkan{};
        auto result = detail::FrameDriverCreateCandidate::create(
            fakeDevice(),
            fakeCommandPool(),
            1,
            detail::FrameDriverCreateOps{}
        );
        CHECK(!result);
        CHECK(isError<err::internal::InvalidArgument>(result.error()));
        CHECK(g_fake.fence_calls == 0);
        CHECK(g_fake.command_buffer_calls == 0);
        CHECK(g_fake.cleanup_count == 0);
        return true;
    }

    bool fenceFailureRollsBackEarlierSlot()
    {
        g_fake = FakeVulkan{};
        auto& fake = g_fake;
        fake.fail_fence_at = 1;

        auto result = detail::FrameDriverCreateCandidate::create(fakeDevice(), fakeCommandPool(), 3, fakeOps());
        CHECK(!result);
        CHECK(isError<err::device::VulkanCallFailed>(result.error()));
        CHECK(result.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        CHECK(fake.fence_calls == 2);
        CHECK(fake.command_buffer_calls == 1);
        CHECK(fake.cleanup_count == 2);
        CHECK(fake.cleanup[0].kind == ECleanup::FREE_COMMAND_BUFFER);
        CHECK(fake.cleanup[0].handle == kCommandBufferBase);
        CHECK(fake.cleanup[1].kind == ECleanup::DESTROY_FENCE);
        CHECK(fake.cleanup[1].handle == kFenceBase);
        return true;
    }

    bool commandBufferFailureRollsBackCurrentFenceAndEarlierSlot()
    {
        g_fake = FakeVulkan{};
        auto& fake = g_fake;
        fake.fail_command_buffer_at = 1;

        auto result = detail::FrameDriverCreateCandidate::create(fakeDevice(), fakeCommandPool(), 3, fakeOps());
        CHECK(!result);
        CHECK(isError<err::device::VulkanCallFailed>(result.error()));
        CHECK(fake.fence_calls == 2);
        CHECK(fake.command_buffer_calls == 2);
        CHECK(fake.cleanup_count == 3);
        CHECK(fake.cleanup[0].kind == ECleanup::DESTROY_FENCE);
        CHECK(fake.cleanup[0].handle == kFenceBase + 1);
        CHECK(fake.cleanup[1].kind == ECleanup::FREE_COMMAND_BUFFER);
        CHECK(fake.cleanup[1].handle == kCommandBufferBase);
        CHECK(fake.cleanup[2].kind == ECleanup::DESTROY_FENCE);
        CHECK(fake.cleanup[2].handle == kFenceBase);
        return true;
    }

    bool successfulCandidateRollsBackUntilReleased()
    {
        g_fake = FakeVulkan{};
        auto& fake = g_fake;
        {
            auto result = detail::FrameDriverCreateCandidate::create(
                fakeDevice(),
                fakeCommandPool(),
                kMaxFramesInFlight,
                fakeOps()
            );
            CHECK(result);
            CHECK(result->fenceCount() == kMaxFramesInFlight);
            CHECK(result->commandBufferCount() == kMaxFramesInFlight);
        }

        CHECK(fake.cleanup_count == 2 * kMaxFramesInFlight);
        for (std::uint32_t reverse = 0; reverse < kMaxFramesInFlight; ++reverse)
        {
            const std::uint32_t slot = kMaxFramesInFlight - 1 - reverse;
            CHECK(fake.cleanup[2 * reverse].kind == ECleanup::FREE_COMMAND_BUFFER);
            CHECK(fake.cleanup[2 * reverse].handle == kCommandBufferBase + slot);
            CHECK(fake.cleanup[2 * reverse + 1].kind == ECleanup::DESTROY_FENCE);
            CHECK(fake.cleanup[2 * reverse + 1].handle == kFenceBase + slot);
        }
        return true;
    }

    bool commitDisarmsRollback()
    {
        g_fake = FakeVulkan{};
        auto& fake = g_fake;
        {
            auto result = detail::FrameDriverCreateCandidate::create(
                fakeDevice(),
                fakeCommandPool(),
                kMaxFramesInFlight,
                fakeOps()
            );
            CHECK(result);
            result->commit();
        }
        CHECK(fake.cleanup_count == 0);
        return true;
    }

    bool semaphoreCandidateRejectsInvalidOps()
    {
        g_fake = FakeVulkan{};
        auto result =
            detail::PresentSemaphoreCreateCandidate::create(fakeDevice(), 3, 2, detail::PresentSemaphoreCreateOps{});
        CHECK(!result);
        CHECK(isError<err::internal::InvalidArgument>(result.error()));
        CHECK(g_fake.semaphore_calls == 0);
        CHECK(g_fake.cleanup_count == 0);
        return true;
    }

    bool semaphoreFailureRollsBackCompletePrefix()
    {
        g_fake = FakeVulkan{};
        auto& fake = g_fake;
        // Three acquire semaphores succeed, then the first present semaphore
        // succeeds and the second present semaphore fails.
        fake.fail_semaphore_at = 4;

        auto result = detail::PresentSemaphoreCreateCandidate::create(fakeDevice(), 3, 3, fakeSemaphoreOps());
        CHECK(!result);
        CHECK(isError<err::device::VulkanCallFailed>(result.error()));
        CHECK(result.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        CHECK(fake.semaphore_calls == 5);
        CHECK(fake.cleanup_count == 4);
        for (std::uint32_t i = 0; i < fake.cleanup_count; ++i)
        {
            CHECK(fake.cleanup[i].kind == ECleanup::DESTROY_SEMAPHORE);
            CHECK(fake.cleanup[i].handle == kSemaphoreBase + 3 - i);
        }
        return true;
    }

    bool semaphoreNullHandleRollsBackAndFailsLoudly()
    {
        g_fake = FakeVulkan{};
        auto& fake = g_fake;
        fake.null_semaphore_at = 2;

        auto result = detail::PresentSemaphoreCreateCandidate::create(fakeDevice(), 3, 1, fakeSemaphoreOps());
        CHECK(!result);
        CHECK(isError<err::device::VulkanObjectCreationFailed>(result.error()));
        CHECK(fake.semaphore_calls == 3);
        CHECK(fake.cleanup_count == 2);
        CHECK(fake.cleanup[0].handle == kSemaphoreBase + 1);
        CHECK(fake.cleanup[1].handle == kSemaphoreBase);
        return true;
    }

    bool semaphoreCandidateCommitIsOnlyPublicationEdge()
    {
        g_fake = FakeVulkan{};
        auto& fake = g_fake;
        {
            auto result = detail::PresentSemaphoreCreateCandidate::create(fakeDevice(), 3, 2, fakeSemaphoreOps());
            CHECK(result);
        }
        CHECK(fake.cleanup_count == 5);
        for (std::uint32_t i = 0; i < fake.cleanup_count; ++i)
        {
            CHECK(fake.cleanup[i].kind == ECleanup::DESTROY_SEMAPHORE);
            CHECK(fake.cleanup[i].handle == kSemaphoreBase + 4 - i);
        }

        g_fake = FakeVulkan{};
        {
            auto result = detail::PresentSemaphoreCreateCandidate::create(fakeDevice(), 3, 2, fakeSemaphoreOps());
            CHECK(result);
            CHECK(result->acquireCount() == 3);
            CHECK(result->presentCount() == 2);
            CHECK(handleValue(result->acquireSemaphore(0)) == kSemaphoreBase);
            CHECK(handleValue(result->presentSemaphore(1)) == kSemaphoreBase + 4);
            result->commit();
        }
        CHECK(fake.semaphore_calls == 5);
        CHECK(fake.cleanup_count == 0);
        return true;
    }

    bool swapchainStatusClassificationPreservesVkResult()
    {
        auto ready = detail::classifySwapchainAcquireResult(VK_SUCCESS, false);
        CHECK(ready);
        CHECK(ready->image_available);
        CHECK(!ready->mark_rebuild);

        auto suboptimal = detail::classifySwapchainAcquireResult(VK_SUBOPTIMAL_KHR, false);
        CHECK(suboptimal);
        CHECK(suboptimal->image_available);
        CHECK(suboptimal->mark_rebuild);

        auto scaled = detail::classifySwapchainAcquireResult(VK_SUBOPTIMAL_KHR, true);
        CHECK(scaled);
        CHECK(scaled->image_available);
        CHECK(!scaled->mark_rebuild);

        auto timeout = detail::classifySwapchainAcquireResult(VK_TIMEOUT, false);
        CHECK(timeout);
        CHECK(!timeout->image_available);
        CHECK(!timeout->mark_rebuild);

        auto out_of_date = detail::classifySwapchainAcquireResult(VK_ERROR_OUT_OF_DATE_KHR, false);
        CHECK(out_of_date);
        CHECK(!out_of_date->image_available);
        CHECK(out_of_date->mark_rebuild);

        auto acquire_lost = detail::classifySwapchainAcquireResult(VK_ERROR_DEVICE_LOST, false);
        CHECK(!acquire_lost);
        CHECK(isError<err::device::VulkanCallFailed>(acquire_lost.error()));
        CHECK(acquire_lost.error().args[0] == encodeVkResult(VK_ERROR_DEVICE_LOST));

        auto present_out_of_date = detail::classifySwapchainPresentResult(VK_ERROR_OUT_OF_DATE_KHR, false);
        CHECK(present_out_of_date);
        CHECK(present_out_of_date->mark_rebuild);

        auto present_scaled = detail::classifySwapchainPresentResult(VK_SUBOPTIMAL_KHR, true);
        CHECK(present_scaled);
        CHECK(!present_scaled->mark_rebuild);

        auto present_lost = detail::classifySwapchainPresentResult(VK_ERROR_DEVICE_LOST, false);
        CHECK(!present_lost);
        CHECK(isError<err::device::VulkanCallFailed>(present_lost.error()));
        CHECK(present_lost.error().args[0] == encodeVkResult(VK_ERROR_DEVICE_LOST));
        return true;
    }

    bool presentQueueWaitPreservesFailureAndValidatesOps()
    {
        g_fake = FakeVulkan{};
        auto open = detail::ensurePresentContextOpen(false);
        CHECK(open);
        auto closed = detail::ensurePresentContextOpen(true);
        CHECK(!closed);
        CHECK(isError<err::internal::InvalidArgument>(closed.error()));

        auto invalid = detail::waitPresentQueueIdle(VkQueue{}, &fakeQueueWaitIdle);
        CHECK(!invalid);
        CHECK(isError<err::internal::InvalidArgument>(invalid.error()));
        CHECK(g_fake.queue_wait_calls == 0);

        invalid = detail::waitPresentQueueIdle(fakeHandle<VkQueue>(0x4000), nullptr);
        CHECK(!invalid);
        CHECK(isError<err::internal::InvalidArgument>(invalid.error()));
        CHECK(g_fake.queue_wait_calls == 0);

        g_fake.queue_wait_result = VK_ERROR_DEVICE_LOST;
        auto failed = detail::waitPresentQueueIdle(fakeHandle<VkQueue>(0x4000), &fakeQueueWaitIdle);
        CHECK(!failed);
        CHECK(isError<err::device::VulkanCallFailed>(failed.error()));
        CHECK(failed.error().args[0] == encodeVkResult(VK_ERROR_DEVICE_LOST));
        CHECK(g_fake.queue_wait_calls == 1);

        g_fake.queue_wait_result = VK_SUCCESS;
        auto succeeded = detail::waitPresentQueueIdle(fakeHandle<VkQueue>(0x4000), &fakeQueueWaitIdle);
        CHECK(succeeded);
        CHECK(g_fake.queue_wait_calls == 2);
        return true;
    }

    bool submittedFrameIsRecordedBeforePresentFailure()
    {
        std::uint32_t submit_count = 0;
        std::uint32_t record_count = 0;
        std::uint32_t present_count = 0;

        auto submit_failure = detail::submitFrameThenRecordAndPresent(
            [&]() -> Expected<void> {
                ++submit_count;
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_DEVICE_LOST));
            },
            [&]() noexcept { ++record_count; },
            [&]() -> Expected<void> {
                ++present_count;
                return {};
            }
        );
        CHECK(!submit_failure);
        CHECK(submit_count == 1);
        CHECK(record_count == 0);
        CHECK(present_count == 0);

        submit_count = 0;
        record_count = 0;
        present_count = 0;
        bool present_observed_record = false;
        auto present_failure = detail::submitFrameThenRecordAndPresent(
            [&]() -> Expected<void> {
                ++submit_count;
                return {};
            },
            [&]() noexcept { ++record_count; },
            [&]() -> Expected<void> {
                ++present_count;
                present_observed_record = (record_count == 1);
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_DEVICE_LOST));
            }
        );
        CHECK(!present_failure);
        CHECK(submit_count == 1);
        CHECK(record_count == 1);
        CHECK(present_count == 1);
        CHECK(present_observed_record);
        CHECK(present_failure.error().args[0] == encodeVkResult(VK_ERROR_DEVICE_LOST));
        return true;
    }

    bool waitFailureIsStructuredAndTerminal()
    {
        g_fake = FakeVulkan{};
        g_fake.wait_result = VK_TIMEOUT;
        auto fence = lux::gapi::vk::Fence::adopt(fakeHandle<VkFence>(kFenceBase));

        auto result = detail::waitFrameFence(fence, fakeDevice(), fakeRuntimeOps());
        CHECK(!result);
        CHECK(isError<err::device::FrameLifecycleCallFailed>(result.error()));
        CHECK(result.error().args[0] == static_cast<std::uint64_t>(detail::EFrameLifecycleCall::SLOT_FENCE_WAIT));
        CHECK(result.error().args[1] == encodeVkResult(VK_TIMEOUT));
        CHECK(g_fake.frame_call_count == 1);
        CHECK(g_fake.frame_calls[0] == EFrameCall::WAIT_FENCE);
        return true;
    }

    bool invalidRuntimeOpsFailBeforeCall()
    {
        g_fake = FakeVulkan{};
        auto fence = lux::gapi::vk::Fence::adopt(fakeHandle<VkFence>(kFenceBase));
        auto command_buffer = lux::gapi::vk::CommandBuffer::adopt(fakeHandle<VkCommandBuffer>(kCommandBufferBase));

        auto wait_result = detail::waitFrameFence(fence, fakeDevice(), detail::FrameDriverRuntimeOps{});
        CHECK(!wait_result);
        CHECK(isError<err::internal::InvalidArgument>(wait_result.error()));

        auto begin_result =
            detail::beginFrameRecording(fence, command_buffer, fakeDevice(), detail::FrameDriverRuntimeOps{});
        CHECK(!begin_result);
        CHECK(isError<err::internal::InvalidArgument>(begin_result.error()));

        std::uint32_t submit_count = 0;
        auto end_result =
            detail::endFrameRecordingThen(command_buffer, detail::FrameDriverRuntimeOps{}, [&]() -> Expected<void> {
                ++submit_count;
                return {};
            }
            );
        CHECK(!end_result);
        CHECK(isError<err::internal::InvalidArgument>(end_result.error()));
        CHECK(submit_count == 0);
        CHECK(g_fake.frame_call_count == 0);
        return true;
    }

    bool recordingFailureStopsAtExactBoundary()
    {
        constexpr std::array<EFrameCall, 3> expected_calls{
            EFrameCall::RESET_FENCE,
            EFrameCall::RESET_COMMAND_BUFFER,
            EFrameCall::BEGIN_COMMAND_BUFFER,
        };
        constexpr std::array<VkResult, 3> failures{
            VK_ERROR_DEVICE_LOST,
            VK_ERROR_OUT_OF_HOST_MEMORY,
            VK_ERROR_OUT_OF_DEVICE_MEMORY,
        };

        for (std::uint32_t failed_stage = 0; failed_stage < failures.size(); ++failed_stage)
        {
            g_fake = FakeVulkan{};
            if (failed_stage == 0)
                g_fake.reset_fence_result = failures[failed_stage];
            else if (failed_stage == 1)
                g_fake.reset_command_buffer_result = failures[failed_stage];
            else
                g_fake.begin_command_buffer_result = failures[failed_stage];

            auto fence = lux::gapi::vk::Fence::adopt(fakeHandle<VkFence>(kFenceBase));
            auto command_buffer = lux::gapi::vk::CommandBuffer::adopt(fakeHandle<VkCommandBuffer>(kCommandBufferBase));
            auto result = detail::beginFrameRecording(fence, command_buffer, fakeDevice(), fakeRuntimeOps());

            CHECK(!result);
            CHECK(isError<err::device::FrameLifecycleCallFailed>(result.error()));
            CHECK(
                result.error().args[0] ==
                static_cast<std::uint64_t>(detail::EFrameLifecycleCall::FENCE_RESET) + failed_stage
            );
            CHECK(result.error().args[1] == encodeVkResult(failures[failed_stage]));
            CHECK(g_fake.frame_call_count == failed_stage + 1);
            for (std::uint32_t i = 0; i <= failed_stage; ++i)
                CHECK(g_fake.frame_calls[i] == expected_calls[i]);
        }
        return true;
    }

    bool recordingSuccessRunsEachStepOnce()
    {
        g_fake = FakeVulkan{};
        auto fence = lux::gapi::vk::Fence::adopt(fakeHandle<VkFence>(kFenceBase));
        auto command_buffer = lux::gapi::vk::CommandBuffer::adopt(fakeHandle<VkCommandBuffer>(kCommandBufferBase));

        auto result = detail::beginFrameRecording(fence, command_buffer, fakeDevice(), fakeRuntimeOps());
        CHECK(result);
        CHECK(g_fake.frame_call_count == 3);
        CHECK(g_fake.frame_calls[0] == EFrameCall::RESET_FENCE);
        CHECK(g_fake.frame_calls[1] == EFrameCall::RESET_COMMAND_BUFFER);
        CHECK(g_fake.frame_calls[2] == EFrameCall::BEGIN_COMMAND_BUFFER);
        return true;
    }

    bool endFailureNeverEntersSubmitStage()
    {
        g_fake = FakeVulkan{};
        g_fake.end_command_buffer_result = VK_ERROR_DEVICE_LOST;
        auto command_buffer = lux::gapi::vk::CommandBuffer::adopt(fakeHandle<VkCommandBuffer>(kCommandBufferBase));
        std::uint32_t submit_count = 0;

        auto result = detail::endFrameRecordingThen(command_buffer, fakeRuntimeOps(), [&]() -> Expected<void> {
            ++submit_count;
            return {};
        }
        );

        CHECK(!result);
        CHECK(isError<err::device::VulkanCallFailed>(result.error()));
        CHECK(result.error().args[0] == encodeVkResult(VK_ERROR_DEVICE_LOST));
        CHECK(g_fake.frame_call_count == 1);
        CHECK(g_fake.frame_calls[0] == EFrameCall::END_COMMAND_BUFFER);
        CHECK(submit_count == 0);
        return true;
    }

    bool successfulEndEntersSubmitStageExactlyOnce()
    {
        g_fake = FakeVulkan{};
        auto command_buffer = lux::gapi::vk::CommandBuffer::adopt(fakeHandle<VkCommandBuffer>(kCommandBufferBase));
        std::uint32_t submit_count = 0;

        auto result = detail::endFrameRecordingThen(command_buffer, fakeRuntimeOps(), [&]() -> Expected<void> {
            ++submit_count;
            return {};
        }
        );

        CHECK(result);
        CHECK(g_fake.frame_call_count == 1);
        CHECK(g_fake.frame_calls[0] == EFrameCall::END_COMMAND_BUFFER);
        CHECK(submit_count == 1);
        return true;
    }
} // namespace

int
main()
{
    if (!invalidFrameCountFailsBeforeVulkan())
        return 1;
    if (!invalidCreateOpsFailBeforeCall())
        return 1;
    if (!fenceFailureRollsBackEarlierSlot())
        return 1;
    if (!commandBufferFailureRollsBackCurrentFenceAndEarlierSlot())
        return 1;
    if (!successfulCandidateRollsBackUntilReleased())
        return 1;
    if (!commitDisarmsRollback())
        return 1;
    if (!semaphoreCandidateRejectsInvalidOps())
        return 1;
    if (!semaphoreFailureRollsBackCompletePrefix())
        return 1;
    if (!semaphoreNullHandleRollsBackAndFailsLoudly())
        return 1;
    if (!semaphoreCandidateCommitIsOnlyPublicationEdge())
        return 1;
    if (!swapchainStatusClassificationPreservesVkResult())
        return 1;
    if (!presentQueueWaitPreservesFailureAndValidatesOps())
        return 1;
    if (!submittedFrameIsRecordedBeforePresentFailure())
        return 1;
    if (!waitFailureIsStructuredAndTerminal())
        return 1;
    if (!invalidRuntimeOpsFailBeforeCall())
        return 1;
    if (!recordingFailureStopsAtExactBoundary())
        return 1;
    if (!recordingSuccessRunsEachStepOnce())
        return 1;
    if (!endFailureNeverEntersSubmitStage())
        return 1;
    if (!successfulEndEntersSubmitStageExactlyOnce())
        return 1;
    return 0;
}
