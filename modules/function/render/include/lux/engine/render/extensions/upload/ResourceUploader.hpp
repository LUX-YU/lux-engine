#pragma once

#include <lux/engine/render/visibility.h>

#include <exec/task.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/extensions/device/SubmissionQueue.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <optional>
#include <span>

namespace lux::render
{
    class ReplicaMutation;
}

namespace lux::render::detail
{
    class DeviceSession;
    class FrameOperation;
    class RenderGraphInstance;
    struct StagingRange;
    class StagingArena;

    struct TransferStats final
    {
        std::uint64_t copied_bytes = 0, copy_regions = 0, prepare_ns = 0, record_submit_ns = 0;
        std::uint64_t prepare_windows = 0, copy_submissions = 0, acquire_submissions = 0;
        std::uint64_t inline_windows = 0;
        std::uint64_t staging_bytes = 0, pending_bytes = 0, pending_peak = 0;
        std::uint64_t gpu_ns = 0, gpu_samples = 0, gpu_query_errors = 0;
        // Optional sampling never backpressures uploads. A Copy pair stays reserved until an
        // existing Graphics submission resolves it; later batches may therefore be unsampled.
        std::uint64_t gpu_samples_skipped = 0;
        bool cpu_timing_enabled = false;
    };

    struct ImageTransferRegion final
    {
        std::uint64_t source_offset = 0, source_row_pitch = 0;
        std::uint32_t block_extent = 1, block_bytes = 4;
        nri::TextureRegionDesc texture{};
    };

    enum class EImageUploadUse : std::uint8_t
    {
        SAMPLED,
        COPY_SOURCE
    };

    struct ImageUploadBoundary final
    {
        EImageUploadUse use = EImageUploadUse::SAMPLED;
        // Operation-owned until terminal delivery. Includes the known prefix on failure.
        SubmissionFrontiers* accepted = nullptr;
    };

    struct BufferWrite final
    {
        nri::Buffer* target = nullptr;
        std::uint64_t offset = 0;
        std::span<const std::byte> source;
    };

    struct BufferZero final
    {
        nri::Buffer* target = nullptr;
        std::uint64_t offset = 0, size = 0;
    };

    // One device's upload facilities. Tasks borrow their source and operation-owned targets; this object
    // owns neither professional candidates nor running tasks. Start with the RenderThread scheduler.
    class LUX_RENDER_PUBLIC ResourceUploader final
    {
    public:
        using Result = RenderResult<void>;
        using Task = exec::task<Result>;

        ~ResourceUploader() noexcept;
        ResourceUploader(const ResourceUploader&) = delete;
        ResourceUploader& operator=(const ResourceUploader&) = delete;

        // Creates staging and timestamp facilities without starting work. The device and CPU execution outlive it.
        [[nodiscard]] static RenderResult<std::unique_ptr<ResourceUploader>>
        create(DeviceSession&, const TransferOptions&, execution::CpuScheduler) noexcept;

        // The shared execution context used for professional CPU preparation before staging.
        [[nodiscard]] execution::CpuScheduler cpuScheduler() const noexcept
        {
            return cpu_scheduler_;
        }

        [[nodiscard]] bool reserveSourceBytes(std::size_t bytes) noexcept;
        void releaseSourceBytes(std::size_t bytes) noexcept;

        // The current graph round reserves staging, without suspending independent transfers.
        void reservePreparationStaging(bool reserve) noexcept
        {
            preparation_reserved_ = reserve;
        }

        [[nodiscard]] bool hasPendingTransfers() const noexcept { return progressing_ || ready_head_ || range_waiters_; }
        [[nodiscard]] bool canProgressTransfers() const noexcept;
        [[nodiscard]] bool canSubmitPreparationCopies() const noexcept;
        [[nodiscard]] Task submitReadyTransfers() noexcept;
        [[nodiscard]] RenderResult<void> flushFrameCopies() noexcept;
        struct FrameCopyReceipt final
        {
            SubmissionPoint point;
            std::size_t ranges;
        };

        // Empty means the queue or command window could not admit work yet.
        [[nodiscard]] exec::task<RenderResult<std::optional<FrameCopyReceipt>>> submitPreparationCopies() noexcept;
        void acceptFrameCopies(FrameCopyReceipt) noexcept;
        void cancelFrameCopies() noexcept;
        [[nodiscard]] bool hasFrameCopies() const noexcept;

        // Lazy device-thread task. Source ranges and their descriptors stay valid through terminal completion.
        // Success means Copy/Acquire acceptance; it does not wait for GPU completion. Partial failures still
        // require the caller to retain or defer-release targets already referenced by accepted work.
        [[nodiscard]] Task
        writeBuffers(std::span<const BufferWrite> writes, std::span<const BufferZero> fills = {}) noexcept;
        // Each source spans the first element through the last element's end, including stride gaps.
        // Pack only element_bytes per element; destination ranges and byte admission use packed sizes.
        // The layout applies to this batch, keeping BufferWrite a compact borrowed range.
        [[nodiscard]] Task writeBuffers(
            std::span<const BufferWrite> writes,
            std::uint32_t element_bytes,
            std::uint32_t source_stride
        ) noexcept;
        [[nodiscard]] Task writeImage(
            nri::Texture& target,
            std::span<const std::byte> source,
            std::vector<ImageTransferRegion> regions,
            ImageUploadBoundary boundary = ImageUploadBoundary{}
        ) noexcept;
        [[nodiscard]] Task cloneImage(
            nri::Texture& target,
            const nri::Texture& original,
            std::span<const std::byte> source,
            std::vector<ImageTransferRegion> regions
        ) noexcept;

        // Reserve an entire write, then copy into owned staging before returning.
        // Capacity failure leaves this write unstaged. Caller bytes may change after success.
        [[nodiscard]] RenderResult<std::size_t> stageBuffer(const BufferWrite&, std::size_t unit = 4) noexcept;

        // Stage the prefix that fits this preparation window. Zero bytes indicate backpressure.
        [[nodiscard]] RenderResult<std::size_t> stageBufferPrefix(const BufferWrite&, std::size_t unit = 4) noexcept;

        [[nodiscard]] std::size_t windowRegionLimit() const noexcept
        {
            return options_.window_regions;
        }

        [[nodiscard]] std::size_t windowByteLimit() const noexcept
        {
            return options_.chunk_bytes * options_.staging_pages;
        }

        [[nodiscard]] TransferStats statistics() const noexcept
        {
            auto result = statistics_;
            result.cpu_timing_enabled = options_.measure_cpu;
            return result;
        }

    private:
        friend class ::lux::render::ReplicaMutation;
        [[nodiscard]] Task writeReplicaBuffers(std::span<const BufferWrite>, SubmissionFrontiers&) noexcept;

        friend class FrameOperation;
        friend class RenderGraphInstance;
        [[nodiscard]] std::size_t stagedFrameCopyCount() const noexcept;
        [[nodiscard]] bool canBeginCommands(const SubmissionQueue&) const noexcept;
        void recordFrameCopies(nri::CommandBuffer&) noexcept;
        struct Copy;
        struct UploadWindow;
        class WindowSender;
        struct Measurement;
        void enqueue(UploadWindow&) noexcept;
        struct FrameCopy;
        std::vector<FrameCopy> frame_copies_;
        std::uint64_t frame_copy_receipt_serial_ = 0;
        ResourceUploader(DeviceSession&, TransferOptions, execution::CpuScheduler) noexcept;
        struct UploadRequest;
        [[nodiscard]] Task write(UploadRequest) noexcept;
        [[nodiscard]] RenderResult<void> collectUploadTimestamps(std::size_t page, std::uint32_t bits) noexcept;

        struct CommandWindow final
        {
            nri::CommandAllocator* allocator = nullptr;
            nri::CommandBuffer* command = nullptr;
            std::uint64_t serial = 0;
        };

        [[nodiscard]] RenderResult<nri::CommandBuffer*> beginCommands(SubmissionQueue&, std::uint32_t&) noexcept;
        void acceptCommands(SubmissionQueue&, std::uint32_t, std::uint64_t) noexcept;
        std::vector<CommandWindow> command_windows_;
        std::vector<std::uint32_t> next_commands_;
        bool progressing_ = false;
        std::unique_ptr<StagingArena> staging_arena_;
        bool preparation_reserved_ = false;
        DeviceSession& device_;
        SubmissionQueue* copy_queue_ = nullptr;
        TransferOptions options_;
        execution::CpuScheduler cpu_scheduler_;
        TransferStats statistics_;
        UploadWindow* ready_head_ = nullptr;
        UploadWindow* ready_tail_ = nullptr;
        UploadWindow* range_waiters_ = nullptr;
        UploadWindow* range_waiters_tail_ = nullptr;
        std::vector<UploadWindow*> batch_entries_;
        std::unique_ptr<Measurement[]> measurements_;
        std::size_t graphics_measurement_base_ = 0;
        std::uint64_t query_result_stride_ = 16;
        std::array<nri::QueryPool*, 2> timestamps_{};
        nri::Buffer* timestamp_results_ = nullptr;
        std::uint64_t* mapped_timestamps_ = nullptr;
        std::uint32_t copy_timestamp_bits_ = 0;
        std::uint32_t graphics_timestamp_bits_ = 0;
    };
} // namespace lux::render::detail
