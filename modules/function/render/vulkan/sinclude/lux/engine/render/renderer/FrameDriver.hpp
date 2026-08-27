#pragma once
/**
 * @file FrameDriver.hpp
 * @brief Centralised per-frame synchronisation — owns the single set of fences,
 *        semaphores, and primary command buffers for the entire render tick.
 *
 * One FrameDriver per Renderer.  All offscreen + swapchain graph recordings
 * share the same primary CB → one vkQueueSubmit per frame.
 *
 * Thread model: all methods are render-thread only.
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/renderer/FrameContext.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/gapi/vk/Fence.hpp>
#include <lux/engine/gapi/vk/Semaphore.hpp>
#include <lux/engine/gapi/vk/CommandBuffer.hpp>
#include <lux/cxx/container/SmallVector.hpp>

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace lux::render
{

    class ResourceContext;
    class PresentContext;
    class SubmissionMerger;   // defined in FrameSubmitMerge.hpp — kept out of this header
    struct RGQueueSubmission; // (forward-decl) so external consumers of FrameDriver don't
                              // transitively pull RGRecorder.hpp -> RGCompiledGraph.hpp.

    namespace detail
    {
        enum class EFrameLifecycleCall : std::uint32_t
        {
            SLOT_FENCE_WAIT = 10u,
            FENCE_RESET = 11u,
            COMMAND_BUFFER_RESET = 12u,
            COMMAND_BUFFER_BEGIN = 13u,
        };

        /// Injectable Vulkan entry points for the construction transaction. The
        /// production factory supplies the real functions; the CPU test supplies
        /// deterministic fakes. Function pointers keep this seam allocation-free
        /// and avoid inheritance, RTTI, locks, and global mutable hooks.
        struct FrameDriverCreateOps final
        {
            PFN_vkCreateFence create_fence{nullptr};
            PFN_vkDestroyFence destroy_fence{nullptr};
            PFN_vkAllocateCommandBuffers allocate_command_buffers{nullptr};
            PFN_vkFreeCommandBuffers free_command_buffers{nullptr};
        };

        /// Injectable frame-slot Vulkan operations. Keeping the sequencing in one
        /// small function makes the fail-fast contract CPU-testable: once any step
        /// fails, no later operation may touch the slot.
        struct FrameDriverRuntimeOps final
        {
            using WaitFenceFn = VkResult (*)(gapi::vk::Fence&, VkDevice) noexcept;
            using ResetFenceFn = VkResult (*)(gapi::vk::Fence&, VkDevice) noexcept;
            using ResetCommandBufferFn = VkResult (*)(gapi::vk::CommandBuffer&) noexcept;
            using BeginCommandBufferFn = VkResult (*)(gapi::vk::CommandBuffer&, VkCommandBufferUsageFlags) noexcept;
            using EndCommandBufferFn = VkResult (*)(gapi::vk::CommandBuffer&) noexcept;

            WaitFenceFn wait_fence{nullptr};
            ResetFenceFn reset_fence{nullptr};
            ResetCommandBufferFn reset_command_buffer{nullptr};
            BeginCommandBufferFn begin_command_buffer{nullptr};
            EndCommandBufferFn end_command_buffer{nullptr};
        };

        /// Wait for the slot's previous submission. Failure means there is no proof
        /// that any slot-local resource is reusable and therefore must be propagated.
        [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<void>
        waitFrameFence(gapi::vk::Fence& fence, VkDevice device, const FrameDriverRuntimeOps& ops) noexcept;

        /// Reset fence → reset command buffer → begin command recording. This is an
        /// ordered transaction with no recovery path inside the current device
        /// session: after the fence reset, returning a fake "skipped frame" would
        /// leave that fence unsignalled forever.
        [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<void> beginFrameRecording(
            gapi::vk::Fence& fence,
            gapi::vk::CommandBuffer& command_buffer,
            VkDevice device,
            const FrameDriverRuntimeOps& ops
        ) noexcept;

        /// End the primary command buffer and only then enter the submit stage.
        /// Keeping the continuation inside this helper makes "end failure means no
        /// queue submit" a directly testable structural guarantee.
        template <typename SubmitFn>
        [[nodiscard]] Expected<void> endFrameRecordingThen(
            gapi::vk::CommandBuffer& command_buffer,
            const FrameDriverRuntimeOps& ops,
            SubmitFn&& submit
        )
        {
            if (ops.end_command_buffer == nullptr)
                return renderFailure<err::internal::InvalidArgument>();

            const VkResult result = ops.end_command_buffer(command_buffer);
            if (result != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
            }
            return std::forward<SubmitFn>(submit)();
        }

        /// Keep the irreversible queue-submit edge separate from presentation.
        /// Once submit succeeds, bookkeeping must be committed even if present
        /// subsequently fails; a present error cannot undo GPU work already queued.
        template <typename SubmitFn, typename RecordFn, typename PresentFn>
        [[nodiscard]] Expected<void>
        submitFrameThenRecordAndPresent(SubmitFn&& submit, RecordFn&& record_submission, PresentFn&& present)
        {
            auto submitted = std::forward<SubmitFn>(submit)();
            if (!submitted)
                return lux::cxx::unexpected<RenderError>(submitted.error());

            std::forward<RecordFn>(record_submission)();
            return std::forward<PresentFn>(present)();
        }

        /// Owns the raw handles while FrameDriver is being assembled. A failed
        /// Vulkan call simply returns from create(); this candidate then unwinds
        /// every handle already created. Ownership is released exactly once, after
        /// the complete FrameDriver object exists and is ready to be published.
        class LUX_FUNCTION_PUBLIC FrameDriverCreateCandidate final
        {
        public:
            ~FrameDriverCreateCandidate() noexcept;

            FrameDriverCreateCandidate(const FrameDriverCreateCandidate&) = delete;
            FrameDriverCreateCandidate& operator=(const FrameDriverCreateCandidate&) = delete;
            FrameDriverCreateCandidate(FrameDriverCreateCandidate&& other) noexcept;
            FrameDriverCreateCandidate& operator=(FrameDriverCreateCandidate&& other) noexcept;

            [[nodiscard]] static Expected<FrameDriverCreateCandidate> create(
                VkDevice device,
                VkCommandPool command_pool,
                std::uint32_t frames_in_flight,
                FrameDriverCreateOps ops
            ) noexcept;

            [[nodiscard]] VkFence fence(std::uint32_t index) const noexcept
            {
                return fences_[index];
            }

            [[nodiscard]] VkCommandBuffer commandBuffer(std::uint32_t index) const noexcept
            {
                return command_buffers_[index];
            }

            [[nodiscard]] std::uint32_t fenceCount() const noexcept
            {
                return fence_count_;
            }
            [[nodiscard]] std::uint32_t commandBufferCount() const noexcept
            {
                return command_buffer_count_;
            }

            /// Disarm rollback after the complete FrameDriver has adopted all
            /// handles. This is the transaction's only publication point.
            void commit() noexcept;

        private:
            using FenceArray = std::array<VkFence, kMaxFramesInFlight>;
            using CommandBufferArray = std::array<VkCommandBuffer, kMaxFramesInFlight>;

            FrameDriverCreateCandidate(VkDevice device, VkCommandPool command_pool, FrameDriverCreateOps ops) noexcept;

            void rollback() noexcept;
            void disarm() noexcept;

            VkDevice device_{VK_NULL_HANDLE};
            VkCommandPool command_pool_{VK_NULL_HANDLE};
            FrameDriverCreateOps ops_{};
            FenceArray fences_{};
            CommandBufferArray command_buffers_{};
            std::uint32_t fence_count_{0};
            std::uint32_t command_buffer_count_{0};
        };
    } // namespace detail

    // =============================================================================
    // FrameDriver
    // =============================================================================

    class LUX_FUNCTION_PUBLIC FrameDriver
    {
    public:
        ~FrameDriver();

        [[nodiscard]] static Expected<std::unique_ptr<FrameDriver>>
        create(ResourceContext& res_ctx, std::uint32_t frames_in_flight);

        FrameDriver(const FrameDriver&) = delete;
        FrameDriver& operator=(const FrameDriver&) = delete;
        FrameDriver(FrameDriver&&) = delete;
        FrameDriver& operator=(FrameDriver&&) = delete;

        // ── Frame lifecycle ─────────────────────────────────────────────────

        /// Wait fence → handle swapchain rebuild → acquire (if present ctx) →
        /// reset + begin primary CB → return a filled FrameRuntime.
        /// 拆层:target 级呈现机件(surface/swapchain/sem 环)在
        /// PresentContext(Surface Target 持有);本类只剩帧级。
        /// @param frame_index   FIF slot index (0 .. frames_in_flight-1)
        /// @param present       Optional — nullptr for headless / offscreen-only ticks.
        /// @param frame_id      Monotonically increasing ID from the server.
        /// @return A FrameRuntime with .primary_cmd ready for recording. A
        ///         minimised swapchain or retryable rebuild/acquire failure is a
        ///         successful invalid FrameRuntime; Vulkan slot-state failures are
        ///         RenderError failures and must stop this device session.
        [[nodiscard]] Expected<FrameRuntime>
        beginFrame(uint32_t frame_index, uint64_t frame_id, PresentContext* present);

        /// End primary CB → submit (with semaphore chain if presenting) → present.
        /// @param rt       The FrameRuntime returned by beginFrame().
        /// @param present  Must be the same pointer passed to beginFrame().
        [[nodiscard]] Expected<void> endFrame(const FrameRuntime& rt, PresentContext* present);

        // ── Utilities ───────────────────────────────────────────────────────

        /// Wait ALL fences — use before resize / rebuild that touches GPU resources
        /// from other FIF slots.
        [[nodiscard]] Expected<void> waitAllFences();

        [[nodiscard]] uint32_t framesInFlight() const noexcept
        {
            return frames_in_flight_;
        }

        /// Highest frame serial whose GPU work is FENCE-PROVEN complete.
        ///
        /// This is the ONLY sound currency for deferred destruction. "current
        /// serial - frames_in_flight" is NOT: the server advances serials on
        /// ticks that never submit (command-only ticks, the no-views early
        /// return), so serial arithmetic over-claims completion — the scene-cycle
        /// stress gate caught retired scenes being destroyed while their command
        /// buffers were still executing. Updated whenever a slot fence is waited
        /// (beginFrame / waitAllFences): the fence proves that slot's LAST
        /// SUBMITTED serial finished, and same-queue FIFO ordering makes the
        /// value monotone over every earlier submission.
        [[nodiscard]] uint64_t gpuCompletedSerial() const noexcept
        {
            return gpu_completed_serial_;
        }

        /// Highest frame serial that was actually SUBMITTED to the GPU. The sound
        /// retire threshold for resources detached from the frame graph at a
        /// known point: nothing after this serial can reference them, and unlike
        /// current_stamp_.serial it is always reachable by the completion
        /// watermark above (serials advance on ticks that never submit — a
        /// threshold taken from an unsubmitted tick is proven by NO fence, ever).
        [[nodiscard]] uint64_t lastSubmittedSerial() const noexcept
        {
            uint64_t s = 0;
            for (uint32_t i = 0; i < frames_in_flight_; ++i)
                s = (std::max)(s, slot_last_submitted_[i]);
            return s;
        }

    private:
        FrameDriver(
            ResourceContext& res_ctx,
            std::uint32_t frames_in_flight,
            const detail::FrameDriverCreateCandidate& candidate,
            std::unique_ptr<SubmissionMerger> submission_merger
        ) noexcept;

        Expected<void> submitRecordedFrame(const FrameRuntime& rt, bool presenting, VkSemaphore present_sem);

        ResourceContext& res_ctx_;
        uint32_t frames_in_flight_;

        // Fence-proven completion bookkeeping (see gpuCompletedSerial()).
        uint64_t gpu_completed_serial_{0};
        uint64_t slot_last_submitted_[kMaxFramesInFlight]{};

        template <typename T> using SmallVec = lux::cxx::SmallVector<T, kMaxFramesInFlight>;

        // (acquire/present 信号量已迁 PresentContext——target 级;拆层。)
        SmallVec<gapi::vk::Fence> fences_;
        SmallVec<gapi::vk::CommandBuffer> cmd_buffers_;

        // Reused per-frame scratch for endFrame() — sized once, then .clear()-reused so
        // the hot submit path performs no heap allocation after warm-up. The merger is
        // held by pointer (forward-declared) so this header doesn't drag RGRecorder ->
        // RGCompiledGraph into render's external consumers (e.g. the ui module).
        std::unique_ptr<SubmissionMerger> submission_merger_;
        std::vector<const RGQueueSubmission*> exec_subs_scratch_;
        std::vector<VkSemaphoreSubmitInfo> retire_waits_scratch_;
        std::vector<VkSemaphoreSubmitInfo> waits_with_acquire_scratch_;
        std::vector<VkSemaphoreSubmitInfo> signals_with_external_scratch_; // graphics signals + external (interop)

        // (ensurePresentSemaphoresForSwapchain / presentImage 已迁
        //  PresentContext::resyncSemaphores / present。)
    };

} // namespace lux::render
