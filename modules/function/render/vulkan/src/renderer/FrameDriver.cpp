#include <lux/engine/render/renderer/FrameDriver.hpp>
#include <lux/engine/render/renderer/FrameSubmitMerge.hpp>
#include <lux/engine/render/renderer/FrameRetirementPlan.hpp>
#include <lux/engine/render/targets/PresentContext.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::render
{
    namespace detail
    {
        [[nodiscard]] RenderError frameLifecycleFailure(EFrameLifecycleCall call, VkResult result) noexcept
        {
            return renderError<err::device::FrameLifecycleCallFailed>(
                static_cast<std::uint32_t>(call),
                encodeVkResult(result)
            );
        }

        Expected<void>
        waitFrameFence(gapi::vk::Fence& fence, VkDevice device, const FrameDriverRuntimeOps& ops) noexcept
        {
            if (ops.wait_fence == nullptr)
                return renderFailure<err::internal::InvalidArgument>();
            const VkResult result = ops.wait_fence(fence, device);
            if (result != VK_SUCCESS)
                return lux::cxx::unexpected<RenderError>(
                    frameLifecycleFailure(EFrameLifecycleCall::SLOT_FENCE_WAIT, result)
                );
            return {};
        }

        Expected<void> beginFrameRecording(
            gapi::vk::Fence& fence,
            gapi::vk::CommandBuffer& command_buffer,
            VkDevice device,
            const FrameDriverRuntimeOps& ops
        ) noexcept
        {
            if (ops.reset_fence == nullptr || ops.reset_command_buffer == nullptr ||
                ops.begin_command_buffer == nullptr)
            {
                return renderFailure<err::internal::InvalidArgument>();
            }

            VkResult result = ops.reset_fence(fence, device);
            if (result != VK_SUCCESS)
                return lux::cxx::unexpected<RenderError>(
                    frameLifecycleFailure(EFrameLifecycleCall::FENCE_RESET, result)
                );

            result = ops.reset_command_buffer(command_buffer);
            if (result != VK_SUCCESS)
                return lux::cxx::unexpected<RenderError>(
                    frameLifecycleFailure(EFrameLifecycleCall::COMMAND_BUFFER_RESET, result)
                );

            result = ops.begin_command_buffer(command_buffer, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
            if (result != VK_SUCCESS)
                return lux::cxx::unexpected<RenderError>(
                    frameLifecycleFailure(EFrameLifecycleCall::COMMAND_BUFFER_BEGIN, result)
                );
            return {};
        }

        FrameDriverCreateCandidate::FrameDriverCreateCandidate(
            VkDevice device,
            VkCommandPool command_pool,
            FrameDriverCreateOps ops
        ) noexcept
            : device_(device), command_pool_(command_pool), ops_(ops)
        {
        }

        FrameDriverCreateCandidate::~FrameDriverCreateCandidate() noexcept
        {
            rollback();
        }

        FrameDriverCreateCandidate::FrameDriverCreateCandidate(FrameDriverCreateCandidate&& other) noexcept
            : device_(std::exchange(other.device_, VkDevice{})),
              command_pool_(std::exchange(other.command_pool_, VkCommandPool{})), ops_(other.ops_),
              fences_(other.fences_), command_buffers_(other.command_buffers_),
              fence_count_(std::exchange(other.fence_count_, 0)),
              command_buffer_count_(std::exchange(other.command_buffer_count_, 0))
        {
            other.fences_ = {};
            other.command_buffers_ = {};
        }

        FrameDriverCreateCandidate& FrameDriverCreateCandidate::operator=(FrameDriverCreateCandidate&& other) noexcept
        {
            if (this == &other)
                return *this;

            rollback();
            device_ = std::exchange(other.device_, VkDevice{});
            command_pool_ = std::exchange(other.command_pool_, VkCommandPool{});
            ops_ = other.ops_;
            fences_ = other.fences_;
            command_buffers_ = other.command_buffers_;
            fence_count_ = std::exchange(other.fence_count_, 0);
            command_buffer_count_ = std::exchange(other.command_buffer_count_, 0);
            other.fences_ = {};
            other.command_buffers_ = {};
            return *this;
        }

        Expected<FrameDriverCreateCandidate> FrameDriverCreateCandidate::create(
            VkDevice device,
            VkCommandPool command_pool,
            std::uint32_t frames_in_flight,
            FrameDriverCreateOps ops
        ) noexcept
        {
            if (frames_in_flight < 1 || frames_in_flight > kMaxFramesInFlight)
            {
                return renderFailure<err::device::InvalidFramesInFlight>(frames_in_flight, kMaxFramesInFlight);
            }
            const bool is_missing_create_fence = ops.create_fence == nullptr;
            const bool is_missing_destroy_fence = ops.destroy_fence == nullptr;
            const bool is_missing_allocate_buffers = ops.allocate_command_buffers == nullptr;
            const bool is_missing_free_buffers = ops.free_command_buffers == nullptr;
            const bool is_invalid_ops = is_missing_create_fence || is_missing_destroy_fence ||
                is_missing_allocate_buffers || is_missing_free_buffers;
            if (is_invalid_ops)
            {
                return renderFailure<err::internal::InvalidArgument>();
            }

            FrameDriverCreateCandidate candidate(device, command_pool, ops);
            const VkFenceCreateInfo fence_info{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            };
            const VkCommandBufferAllocateInfo command_buffer_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = command_pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };

            for (std::uint32_t i = 0; i < frames_in_flight; ++i)
            {
                VkFence fence = VK_NULL_HANDLE;
                const VkResult fence_result = ops.create_fence(device, &fence_info, nullptr, &fence);
                if (fence_result != VK_SUCCESS)
                {
                    return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(fence_result));
                }
                if (fence == VK_NULL_HANDLE)
                    return renderFailure<err::device::VulkanObjectCreationFailed>();

                candidate.fences_[i] = fence;
                ++candidate.fence_count_;

                VkCommandBuffer command_buffer = VK_NULL_HANDLE;
                const VkResult command_buffer_result =
                    ops.allocate_command_buffers(device, &command_buffer_info, &command_buffer);
                if (command_buffer_result != VK_SUCCESS)
                {
                    return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(command_buffer_result));
                }
                if (command_buffer == VK_NULL_HANDLE)
                    return renderFailure<err::device::VulkanObjectCreationFailed>();

                candidate.command_buffers_[i] = command_buffer;
                ++candidate.command_buffer_count_;
            }

            return Expected<FrameDriverCreateCandidate>{std::move(candidate)};
        }

        void FrameDriverCreateCandidate::commit() noexcept
        {
            disarm();
        }

        void FrameDriverCreateCandidate::disarm() noexcept
        {
            device_ = VK_NULL_HANDLE;
            command_pool_ = VK_NULL_HANDLE;
            fences_ = {};
            command_buffers_ = {};
            fence_count_ = 0;
            command_buffer_count_ = 0;
        }

        void FrameDriverCreateCandidate::rollback() noexcept
        {
            std::uint32_t count = (std::max)(fence_count_, command_buffer_count_);
            while (count > 0)
            {
                const std::uint32_t i = --count;
                if (i < command_buffer_count_ && command_buffers_[i] != VK_NULL_HANDLE)
                {
                    ops_.free_command_buffers(device_, command_pool_, 1, &command_buffers_[i]);
                }
                if (i < fence_count_ && fences_[i] != VK_NULL_HANDLE)
                    ops_.destroy_fence(device_, fences_[i], nullptr);
            }
            disarm();
        }
    } // namespace detail

    namespace
    {
        // SubmitMergeKey / SubmitMergeKeyHasher / SubmissionMerger live in
        // FrameSubmitMerge.hpp now (pure, unit-tested merge logic).

        const detail::FrameDriverCreateOps kFrameDriverCreateOps{
            &vkCreateFence,
            &vkDestroyFence,
            &vkAllocateCommandBuffers,
            &vkFreeCommandBuffers,
        };

        const detail::FrameDriverRuntimeOps kFrameDriverRuntimeOps{
            .wait_fence = [](gapi::vk::Fence& fence, VkDevice device) noexcept { return fence.wait(device); },
            .reset_fence = [](gapi::vk::Fence& fence, VkDevice device) noexcept { return fence.reset(device); },
            .reset_command_buffer =
                [](gapi::vk::CommandBuffer& command_buffer) noexcept { return command_buffer.reset(); },
            .begin_command_buffer =
                [](gapi::vk::CommandBuffer& command_buffer, VkCommandBufferUsageFlags flags) noexcept {
                    return command_buffer.begin(flags);
                },
            .end_command_buffer = [](gapi::vk::CommandBuffer& command_buffer) noexcept { return command_buffer.end(); },
        };

        [[nodiscard]] VkQueue selectQueue(ResourceContext& res_ctx, ERGQueueType queue_type)
        {
            auto& dev_ctx = res_ctx.deviceContext();
            switch (queue_type)
            {
            case ERGQueueType::COMPUTE:
                return dev_ctx.asyncComputeQueue();
            case ERGQueueType::TRANSFER:
                return dev_ctx.transferQueue();
            case ERGQueueType::GRAPHICS:
            default:
                return dev_ctx.graphicsQueue();
            }
        }

        [[nodiscard]] std::mutex& selectQueueMutex(ResourceContext& res_ctx, ERGQueueType queue_type)
        {
            auto& dev_ctx = res_ctx.deviceContext();
            switch (queue_type)
            {
            case ERGQueueType::COMPUTE:
                return dev_ctx.asyncComputeQueueMutex();
            case ERGQueueType::TRANSFER:
                return dev_ctx.transferQueueMutex();
            case ERGQueueType::GRAPHICS:
            default:
                return dev_ctx.graphicsQueueMutex();
            }
        }
    } // namespace

    // =============================================================================
    // Construction / Destruction (RAII)
    // =============================================================================
    Expected<std::unique_ptr<FrameDriver>> FrameDriver::create(ResourceContext& res_ctx, std::uint32_t frames_in_flight)
    {
        auto candidate = detail::FrameDriverCreateCandidate::create(
            res_ctx.deviceContext().logicalDevice(),
            res_ctx.commandPool(),
            frames_in_flight,
            kFrameDriverCreateOps
        );
        if (!candidate)
            return lux::cxx::unexpected<RenderError>(candidate.error());

        auto submission_merger = std::make_unique<SubmissionMerger>();
        auto result = std::unique_ptr<FrameDriver>(
            new FrameDriver(res_ctx, frames_in_flight, *candidate, std::move(submission_merger))
        );
        candidate->commit();
        return Expected<std::unique_ptr<FrameDriver>>{std::move(result)};
    }

    FrameDriver::FrameDriver(
        ResourceContext& res_ctx,
        std::uint32_t frames_in_flight,
        const detail::FrameDriverCreateCandidate& candidate,
        std::unique_ptr<SubmissionMerger> submission_merger
    ) noexcept
        : res_ctx_(res_ctx), frames_in_flight_(frames_in_flight), submission_merger_(std::move(submission_merger))
    {
        for (uint32_t i = 0; i < frames_in_flight; ++i)
        {
            fences_.emplace_back(gapi::vk::Fence::adopt(candidate.fence(i)));
            cmd_buffers_.emplace_back(gapi::vk::CommandBuffer::adopt(candidate.commandBuffer(i)));
        }
    }

    FrameDriver::~FrameDriver()
    {
        auto& dev = res_ctx_.deviceContext().logicalDevice();
        const VkResult idle = dev.waitIdle();
        if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST)
            renderFatal("FrameDriver failed to wait for device idle during teardown");

        for (auto& f : fences_)
            f.release(dev);
        for (auto& cb : cmd_buffers_)
            cb.release(dev, res_ctx_.commandPool());
    }

    // (ensurePresentSemaphoresForSwapchain 已迁 PresentContext::resyncSemaphores。)

    // =============================================================================
    // beginFrame
    // =============================================================================

    Expected<FrameRuntime> FrameDriver::beginFrame(uint32_t frame_index, uint64_t frame_id, PresentContext* present)
    {
        // Never fold an invalid slot with modulo: aliasing two logical slots onto
        // one fence/command buffer would hide the caller bug and corrupt the frame
        // protocol. The server reports this structured failure and stops.
        if (frame_index >= frames_in_flight_)
            return renderFailure<err::internal::InvalidArgument>();

        auto& dev = res_ctx_.deviceContext().logicalDevice();
        const auto fi = frame_index;

        // 1. Wait for the previous frame on this FIF slot. The fence proves the
        //    slot's LAST SUBMITTED serial finished on the GPU — advance the
        //    fence-proven completion watermark (see gpuCompletedSerial()).
        //
        //    A FAILED wait must not fall through. Without the fence we have no
        //    proof the GPU is done with this slot, and everything below reuses
        //    the slot's command buffer, descriptors and images — so continuing
        //    is a data race against the GPU, not a degraded frame. This is a
        //    device-session failure, distinct from a retryable swapchain skip.
        //
        //    VK_TIMEOUT is reachable despite the wait being UINT64_MAX: drivers
        //    cap it internally (Adreno 830 returns VK_TIMEOUT after ~3 s).
        //    Observed on device when a stalled submission never completed —
        //    and before this check the engine happily rendered a hundred more
        //    frames onto the wedged slot.
        auto waited = detail::waitFrameFence(fences_[fi], dev, kFrameDriverRuntimeOps);
        if (!waited)
            return lux::cxx::unexpected<RenderError>(waited.error());
        gpu_completed_serial_ = (std::max)(gpu_completed_serial_, slot_last_submitted_[fi]);

        // 2. Handle pending swapchain rebuild(重建要等全 fence——帧级职责,
        //    所以编排留在这里;机件在 PresentContext)。
        if (present && present->needsRebuild())
        {
            auto all_waited = waitAllFences();
            if (!all_waited)
                return lux::cxx::unexpected<RenderError>(all_waited.error());
            auto r = present->rebuild();
            if (!r)
            {
                if (!detail::isRetryableSwapchainFailure(r.error()))
                    return lux::cxx::unexpected<RenderError>(r.error());
                return FrameRuntime{};
            }
        }

        // 3. Acquire swapchain image (if presenting).
        PresentContext::Acquired acquired{};
        if (present)
        {
            auto acquired_result = present->acquire();
            if (!acquired_result)
            {
                return lux::cxx::unexpected<RenderError>(acquired_result.error());
            }
            acquired = *acquired_result;
            if (!acquired.valid)
            {
                // Only rebuild if the provider explicitly reports rebuild need.
                if (!present->needsRebuild())
                    return FrameRuntime{};

                auto all_waited = waitAllFences();
                if (!all_waited)
                    return lux::cxx::unexpected<RenderError>(all_waited.error());
                auto r = present->rebuild();
                if (!r)
                {
                    if (!detail::isRetryableSwapchainFailure(r.error()))
                        return lux::cxx::unexpected<RenderError>(r.error());
                    return FrameRuntime{};
                }

                acquired_result = present->acquire();
                if (!acquired_result)
                {
                    return lux::cxx::unexpected<RenderError>(acquired_result.error());
                }
                acquired = *acquired_result;
                if (!acquired.valid)
                    return FrameRuntime{};
            }
        }

        // 4. Reset fence + command buffer + begin recording.
        auto recording = detail::beginFrameRecording(fences_[fi], cmd_buffers_[fi], dev, kFrameDriverRuntimeOps);
        if (!recording)
            return lux::cxx::unexpected<RenderError>(recording.error());

        // 5. Populate FrameRuntime.
        FrameRuntime rt{};
        rt.primary_cmd = cmd_buffers_[fi];
        rt.stamp.serial = frame_id;
        rt.stamp.slot = static_cast<FrameSlot>(fi);
        rt.stamp.frames_in_flight = frames_in_flight_;

        if (present)
        {
            rt.image_index = acquired.image_index;
            rt.stamp.image_index = acquired.image_index;
            rt.present_image = acquired.image;
            rt.present_view = acquired.view;
            rt.present_extent = acquired.extent;
            rt.is_present_frame = true;
            rt.present_acquire_sem = acquired.acquire_sem;
            rt.present_signal_sem = acquired.present_sem;
        }

        return rt;
    }

    // =============================================================================
    // endFrame
    // =============================================================================

    Expected<void> FrameDriver::endFrame(const FrameRuntime& rt, PresentContext* present)
    {
        // No primary CB ⇒ this serial never reaches the GPU (skipped frame) —
        // it must NOT enter the fence-proven completion bookkeeping.
        if (rt.primary_cmd == VK_NULL_HANDLE)
            return {};
        if (rt.stamp.slotIndex() >= frames_in_flight_)
            return renderFailure<err::internal::InvalidArgument>();

        const auto fi = rt.stamp.slotIndex();
        VkSemaphore present_sem = VK_NULL_HANDLE;
        if (present)
        {
            // beginFrame 的 acquire 已把两个信号量随 FrameRuntime 下发。
            present_sem = rt.present_signal_sem;
            if (present_sem == VK_NULL_HANDLE || rt.present_acquire_sem == VK_NULL_HANDLE)
                return renderFailure<err::internal::Unspecified>();
        }

        return detail::endFrameRecordingThen(cmd_buffers_[fi], kFrameDriverRuntimeOps, [&]() -> Expected<void> {
            return detail::submitFrameThenRecordAndPresent(
                [&]() -> Expected<void> { return submitRecordedFrame(rt, present != nullptr, present_sem); },
                [&]() noexcept {
                    // The queue submission carrying this slot's fence is
                    // irreversible. Record it before present, whose failure
                    // cannot make the submitted GPU work disappear.
                    slot_last_submitted_[fi] = rt.stamp.serial;
                },
                [&]() -> Expected<void> {
                    if (present == nullptr)
                        return {};
                    return present->present(rt.image_index, present_sem);
                }
            );
        }
        );
    }

    Expected<void> FrameDriver::submitRecordedFrame(const FrameRuntime& rt, bool presenting, VkSemaphore present_sem)
    {
        const auto fi = rt.stamp.slotIndex();
        VkCommandBuffer cmd = cmd_buffers_[fi];

        if (!rt.multi_queue_submits.empty())
        {
            // Merge submissions from all rendered views by (queue_type, cmd) — avoids
            // O(N^2) linear searches and redundant submits in the hot path. The merger
            // owns reused scratch; the returned ref is valid until its next merge().
            const std::vector<RGQueueSubmission>& merged_subs = submission_merger_->merge(rt.multi_queue_submits);

            if (!merged_subs.empty())
            {
                std::vector<const RGQueueSubmission*>& executable_subs = exec_subs_scratch_;
                executable_subs.clear();
                executable_subs.reserve(merged_subs.size());
                for (const auto& sub : merged_subs)
                {
                    if (sub.cmd == VK_NULL_HANDLE)
                        continue;
                    executable_subs.push_back(&sub);
                }
                if (executable_subs.empty())
                    return renderFailure<err::internal::Unspecified>();

                // The frame fence must only signal after ALL of the frame's work is
                // GPU-complete — and in particular after the GRAPHICS queue, which owns
                // the frame's command buffers / present. Attaching it to "the last
                // submission in the host array" is only safe when every submission is on
                // the graphics queue: with independent async queues the last submission is
                // typically compute/transfer and its fence can signal while graphics is
                // still in flight (frame slot reused mid-flight → data race / DEVICE_LOST).
                // So the fast in-loop fence placement is taken ONLY for the all-graphics
                // case; any non-graphics submission falls through to the graphics retire
                // submit below, which carries the fence and is queue-ordered after all
                // prior graphics work.
                std::vector<VkSemaphoreSubmitInfo>& retire_waits = retire_waits_scratch_;
                retire_waits.reserve(16);
                const auto retirement = analyzeFrameRetirement(
                    std::span<const RGQueueSubmission* const>{executable_subs.data(), executable_subs.size()},
                    presenting,
                    retire_waits
                );

                for (size_t si = 0; si < executable_subs.size(); ++si)
                {
                    const auto& sub = *executable_subs[si];

                    const bool is_graphics = (sub.queue_type == ERGQueueType::GRAPHICS);
                    const bool inject_ext_waits = is_graphics && !rt.external_graphics_waits.empty();
                    const bool inject_ext_signals = is_graphics && !rt.external_graphics_signals.empty();

                    std::vector<VkSemaphoreSubmitInfo>& waits_with_acquire = waits_with_acquire_scratch_;
                    waits_with_acquire.clear();
                    const VkSemaphoreSubmitInfo* wait_infos = sub.wait_semaphores.data();
                    uint32_t wait_count = static_cast<uint32_t>(sub.wait_semaphores.size());
                    const auto& signals = sub.signal_semaphores;

                    if (is_graphics && (presenting || inject_ext_waits))
                    {
                        waits_with_acquire.reserve(sub.wait_semaphores.size() + 1u + rt.external_graphics_waits.size());
                        waits_with_acquire.insert(
                            waits_with_acquire.end(),
                            sub.wait_semaphores.begin(),
                            sub.wait_semaphores.end()
                        );
                        if (presenting)
                        {
                            VkSemaphoreSubmitInfo acquire_wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                            acquire_wait.semaphore = rt.present_acquire_sem;
                            acquire_wait.value = 0;
                            acquire_wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                            waits_with_acquire.push_back(acquire_wait);
                        }
                        if (inject_ext_waits) // external (e.g. CUDA) waits before graphics samples
                            waits_with_acquire.insert(
                                waits_with_acquire.end(),
                                rt.external_graphics_waits.begin(),
                                rt.external_graphics_waits.end()
                            );

                        wait_infos = waits_with_acquire.data();
                        wait_count = static_cast<uint32_t>(waits_with_acquire.size());
                    }

                    // Submit signals = this submission's signals [+ external interop signals
                    // on the graphics queue]. `signals` (the raw list) is still used for the
                    // retire-wait bookkeeping below, so external signals are kept separate.
                    const VkSemaphoreSubmitInfo* signal_infos = signals.data();
                    uint32_t signal_count = static_cast<uint32_t>(signals.size());
                    if (inject_ext_signals)
                    {
                        std::vector<VkSemaphoreSubmitInfo>& sig = signals_with_external_scratch_;
                        sig.clear();
                        sig.reserve(signals.size() + rt.external_graphics_signals.size());
                        sig.insert(sig.end(), signals.begin(), signals.end());
                        sig.insert(sig.end(), rt.external_graphics_signals.begin(), rt.external_graphics_signals.end());
                        signal_infos = sig.data();
                        signal_count = static_cast<uint32_t>(sig.size());
                    }

                    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                    cmd_info.commandBuffer = sub.cmd;

                    VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                    submit2.waitSemaphoreInfoCount = wait_count;
                    submit2.pWaitSemaphoreInfos = wait_infos;
                    submit2.commandBufferInfoCount = 1;
                    submit2.pCommandBufferInfos = &cmd_info;
                    submit2.signalSemaphoreInfoCount = signal_count;
                    submit2.pSignalSemaphoreInfos = signal_infos;

                    VkFence submit_fence = VK_NULL_HANDLE;
                    if (retirement.fence_on_last_submission && si + 1 == executable_subs.size())
                        submit_fence = fences_[fi];

                    const std::scoped_lock queue_lock(selectQueueMutex(res_ctx_, sub.queue_type));
                    VkResult sub_res = vkQueueSubmit2(selectQueue(res_ctx_, sub.queue_type), 1, &submit2, submit_fence);
                    if (sub_res != VK_SUCCESS)
                    {
                        return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(sub_res));
                    }
                }

                if (retirement.fence_on_last_submission)
                {
                    // All-graphics frame: the in-loop fence on the last (graphics)
                    // submission already represents whole-frame completion. Frames with
                    // any non-graphics submission never reach here — the guard above
                    // routes them to the graphics retire submit below so the fence lands
                    // on the graphics queue.
                    return {};
                }

                if (retirement.wait_compute_idle)
                {
                    // This compute submission exposed no signal that the graphics
                    // retire submit can join. Other submissions having signals does
                    // not make this one safe, so wait this queue explicitly.
                    auto& dc = res_ctx_.deviceContext();
                    VkResult async_compute_wait{VK_ERROR_UNKNOWN};
                    {
                        const std::scoped_lock queue_lock(dc.asyncComputeQueueMutex());
                        async_compute_wait = vkQueueWaitIdle(dc.asyncComputeQueue());
                    }
                    if (async_compute_wait != VK_SUCCESS)
                    {
                        return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(async_compute_wait));
                    }
                }
                if (retirement.wait_transfer_idle)
                {
                    auto& dc = res_ctx_.deviceContext();
                    VkResult transfer_wait{VK_ERROR_UNKNOWN};
                    {
                        const std::scoped_lock queue_lock(dc.transferQueueMutex());
                        transfer_wait = vkQueueWaitIdle(dc.transferQueue());
                    }
                    if (transfer_wait != VK_SUCCESS)
                    {
                        return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(transfer_wait));
                    }
                }

                std::vector<VkSemaphoreSubmitInfo> retire_signals;
                if (presenting)
                {
                    VkSemaphoreSubmitInfo present_signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                    present_signal.semaphore = present_sem;
                    present_signal.value = 0;
                    present_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
                    retire_signals.push_back(present_signal);
                }

                VkSubmitInfo2 retire_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                retire_submit.waitSemaphoreInfoCount = static_cast<uint32_t>(retire_waits.size());
                retire_submit.pWaitSemaphoreInfos = retire_waits.data();
                retire_submit.signalSemaphoreInfoCount = static_cast<uint32_t>(retire_signals.size());
                retire_submit.pSignalSemaphoreInfos = retire_signals.data();

                const std::scoped_lock queue_lock(res_ctx_.deviceContext().graphicsQueueMutex());
                VkResult retire_res = vkQueueSubmit2(res_ctx_.graphicsQueue(), 1, &retire_submit, fences_[fi]);
                if (retire_res != VK_SUCCESS)
                {
                    return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(retire_res));
                }

                return {};
            }
        }

        // Interop single-queue path: external timeline waits/signals carry values and
        // need sync2. Taken ONLY when a feature injected external sync; otherwise the
        // existing VkSubmitInfo paths below run byte-for-byte (zero regression).
        if (!rt.external_graphics_waits.empty() || !rt.external_graphics_signals.empty())
        {
            std::vector<VkSemaphoreSubmitInfo>& waits = waits_with_acquire_scratch_;
            waits.clear();
            waits.insert(waits.end(), rt.external_graphics_waits.begin(), rt.external_graphics_waits.end());
            std::vector<VkSemaphoreSubmitInfo>& sigs = signals_with_external_scratch_;
            sigs.clear();
            sigs.insert(sigs.end(), rt.external_graphics_signals.begin(), rt.external_graphics_signals.end());
            if (presenting)
            {
                VkSemaphoreSubmitInfo aw{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                aw.semaphore = rt.present_acquire_sem;
                aw.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                waits.push_back(aw);
                VkSemaphoreSubmitInfo ps{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                ps.semaphore = present_sem;
                ps.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
                sigs.push_back(ps);
            }
            VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            ci.commandBuffer = cmd;
            VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            submit2.waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size());
            submit2.pWaitSemaphoreInfos = waits.data();
            submit2.commandBufferInfoCount = 1;
            submit2.pCommandBufferInfos = &ci;
            submit2.signalSemaphoreInfoCount = static_cast<uint32_t>(sigs.size());
            submit2.pSignalSemaphoreInfos = sigs.data();
            const std::scoped_lock queue_lock(res_ctx_.deviceContext().graphicsQueueMutex());
            VkResult r = vkQueueSubmit2(res_ctx_.graphicsQueue(), 1, &submit2, fences_[fi]);
            if (r != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(r));
            }
            return {};
        }

        if (presenting)
        {
            // Swapchain path: wait acquire_sem → draw → signal present_sem + fence.
            VkSemaphore wait_sems[] = {rt.present_acquire_sem};
            VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
            VkSemaphore signal_sems[] = {present_sem};

            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = wait_sems;
            submit.pWaitDstStageMask = wait_stages;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = signal_sems;

            const std::scoped_lock queue_lock(res_ctx_.deviceContext().graphicsQueueMutex());
            VkResult r = vkQueueSubmit(res_ctx_.graphicsQueue(), 1, &submit, fences_[fi]);
            if (r != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(r));
            }
        }
        else
        {
            // Offscreen-only path: submit with fence, no semaphores.
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;

            const std::scoped_lock queue_lock(res_ctx_.deviceContext().graphicsQueueMutex());
            VkResult r = vkQueueSubmit(res_ctx_.graphicsQueue(), 1, &submit, fences_[fi]);
            if (r != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(r));
            }
        }

        return {};
    }

    // (presentImage 已迁 PresentContext::present——含 OUT_OF_DATE/SUBOPTIMAL/
    //  SURFACE_LOST 归一为重建标记的语义。)

    // =============================================================================
    // waitAllFences
    // =============================================================================

    Expected<void> FrameDriver::waitAllFences()
    {
        auto& dev = res_ctx_.deviceContext().logicalDevice();
        for (auto& f : fences_)
        {
            auto waited = detail::waitFrameFence(f, dev, kFrameDriverRuntimeOps);
            if (!waited)
                return lux::cxx::unexpected<RenderError>(waited.error());
        }

        // Advance only after every slot supplied proof. A partial watermark is
        // unusable to rebuild shared swapchain state safely.
        for (uint32_t i = 0; i < frames_in_flight_; ++i)
            gpu_completed_serial_ = (std::max)(gpu_completed_serial_, slot_last_submitted_[i]);
        return {};
    }

} // namespace lux::render
