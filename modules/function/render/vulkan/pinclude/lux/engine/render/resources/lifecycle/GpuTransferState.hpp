#pragma once

#include <atomic>
#include <lux/cxx/concurrent/LockFreeQueue.hpp>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp>
#include <mutex>
#include <vector>

namespace lux::render
{
    // Stable worker backing. Native acquisition is complete before construction;
    // the pipeline owns the physical thread and joins it before releasing this state.
    struct GpuTransferPipeline::State final
    {
        struct CmdPoolSlot;
        State(
            const Config& config,
            SemaphoreOwner semaphore,
            std::unique_ptr<CmdPoolSlot[]> pools,
            std::uint32_t pool_count
        ) noexcept;
        ~State();
        State(const State&) = delete;
        State& operator=(const State&) = delete;
        State(State&&) = delete;
        State& operator=(State&&) = delete;

        bool submitMeshTransfer(MeshTransferTask task);
        bool submitTextureTransfer(TextureTransferTask task);
        bool submitCubeTransfer(CubeTransferTask task);
        uint32_t drainResults(TransferCompletion* out, uint32_t max);
        std::optional<std::uint64_t> submitGraphicsFinalize(VkCommandBuffer command_buffer);
        void releaseAfterGraphicsAcquire(uint32_t batch_slot) noexcept;
        bool needsQueueFamilyOwnershipTransfer() const noexcept;

        enum class EBatchSlotState : std::uint8_t
        {
            FREE,
            RECORDING,
            RECORDED,
            SUBMITTED
        };

        // Android libc++ intentionally disables its incomplete C++20 stop-token
        // implementation.  This worker only needs a shared one-way stop bit, so
        // keep that narrow contract local instead of depending on std::jthread.
        struct TransferStopToken
        {
            const std::atomic<bool>* requested{nullptr};

            [[nodiscard]] bool stopRequested() const noexcept
            {
                return requested != nullptr && requested->load(std::memory_order_acquire);
            }
        };

        [[nodiscard]] BatchSlotLease acquireBatchSlot();
        void releaseBatchSlot(BatchSlotLease slot) noexcept;
        [[nodiscard]] bool retireOneSubmittedSlot();
        [[nodiscard]] bool retireGraphicsFinalize();
        void workerLoop(TransferStopToken stop_token);
        void processMeshTransfer(MeshTransferTask task, TransferStopToken stop_token);
        void processTextureTransfer(TextureTransferTask task, TransferStopToken stop_token);
        void processCubeTransfer(CubeTransferTask task, TransferStopToken stop_token);
        [[nodiscard]] bool publishResult(VGpuTransferResult result);
        void publishRecorded(RecordedBatch batch);

        // Internal helpers for common worker patterns.
        struct StagingResult
        {
            StagingBuffer owner;
            void* mapped{nullptr};
        };

        StagingResult allocStagingBuffer(VkDeviceSize bytes);

        /// Destroy the GPU resources held by a completion that will never be
        /// finalized (a packet dropped on a closed pending-submit ring, or left
        /// un-submitted at shutdown). Frees staging + (for textures) the
        /// per-task image/view/sampler so teardown leaks nothing.
        void freeUnsubmittedCompletion(TransferCompletion& c);

        /// Publish a FAILURE terminal state for a worker task that bailed out
        /// (cancel / invalid data / unknown format / Vulkan/VMA/staging failure).
        /// Carries only the reply identity + reserved slot — NO GPU objects (the
        /// worker destroys its own partials first) — so the render thread settles
        /// the client request with status!=0 and reclaims the reserved bindless
        /// slot, instead of leaking it and hanging the request forever. Meshes
        /// pass slot_index = mesh_index.
        void pushFailure(
            TransferCompletion::EKind kind,
            uint32_t request_id,
            uint32_t slot_index,
            uint32_t resource_gen,
            uint32_t logical_base_mip = 0u
        );
        void notifyLifecycle(
            std::uint32_t request_id,
            TransferCompletion::EKind kind,
            std::uint32_t resource_handle,
            std::uint32_t resource_gen,
            EUploadLifecycleState state
        ) noexcept;

        // Convert engine pixel format to VkFormat; std::nullopt for a format the
        // upload path does not know (never silently substitutes RGBA8).
        static std::optional<VkFormat> toVkFormat(EPixelFormat fmt);

        // ── Members ─────────────────────────────────────────────────────

        lux::cxx::SpscLockFreeRingQueue<VUploadJob> jobs_;
        lux::cxx::SpscLockFreeRingQueue<VGpuTransferResult> results_;
        std::atomic<bool> stop_requested_{false};
        std::atomic<bool> accepting_{true};
        std::atomic<bool> worker_running_{false};
        std::atomic<std::uint64_t> job_epoch_{0};
        std::atomic<std::uint64_t> result_space_epoch_{0};
        std::atomic<std::uint64_t> worker_epoch_{0};
        std::atomic<std::uint64_t> graphics_finalize_timeline_{0};
        std::atomic<std::uint64_t> staging_copied_bytes_{0};
        std::vector<TransferCompletion> shutdown_completions_;
        std::size_t shutdown_completion_cursor_{0};

        // Queue ownership is mode-dependent and based on actual handles. The
        // narrow DeviceContext queue gates provide Vulkan's required external
        // synchronization without protecting any engine/business state.
        VkQueue transfer_queue_{VK_NULL_HANDLE};
        VkQueue graphics_queue_{VK_NULL_HANDLE};
        std::mutex* transfer_queue_mutex_{nullptr};
        std::mutex* graphics_queue_mutex_{nullptr};
        uint32_t transfer_family_{};
        uint32_t graphics_family_{};
        bool needs_ownership_transfer_{false};

        // Both queue owners allocate values from the same timeline. The atomic
        // counter is the cross-thread value allocator; every submit additionally
        // waits for N-1 before signalling N because host allocation order alone
        // does not impose execution order between the transfer/graphics queues.
        // Vulkan queue access itself remains single-owner.
        SemaphoreOwner timeline_sem_;
        std::atomic<uint64_t> timeline_counter_{0};

        // Fixed batch slots. Only the transfer thread records/resets pools; in
        // RECORD_ONLY mode the render thread changes RECORDED→SUBMITTED after
        // its queue submit. Slot state is the only cross-thread coordination.
        struct CmdPoolSlot
        {
            CommandPoolOwner pool;
            std::atomic<EBatchSlotState> state{EBatchSlotState::FREE};
            std::atomic<uint64_t> last_timeline_value{0};
        };

        std::unique_ptr<CmdPoolSlot[]> cmd_pools_;
        uint32_t cmd_pool_count_{0};
        uint32_t next_cmd_pool_{0};

        EGpuTransferMode mode_{EGpuTransferMode::RECORD_ONLY};
        bool can_record_transfer_{false};
        Config::NotifyWorkFn notify_work_{nullptr};
        void* notify_work_state_{nullptr};
        Config::LifecycleFn lifecycle_{nullptr};
        void* lifecycle_state_{nullptr};
        VmaAllocator vma_{nullptr};
        VkDevice device_{VK_NULL_HANDLE};
        DeviceContext& device_context_;
        bool shutdown_complete_{false};
    };
} // namespace lux::render
