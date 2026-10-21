#pragma once

#include <lux/engine/render/visibility.h>

#include <exec/task.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <span>

namespace lux::render::detail
{
    class DeviceSession;
    struct StagingRange;
    class StagingArena;

    struct TransferStats final
    {
        std::uint64_t copied_bytes = 0, copy_regions = 0, prepare_ns = 0, record_submit_ns = 0;
        std::uint64_t prepare_windows = 0, copy_submissions = 0, acquire_submissions = 0;
        std::uint64_t staging_bytes = 0, pending_bytes = 0, pending_peak = 0;
        std::uint64_t gpu_ns = 0, gpu_samples = 0, gpu_query_errors = 0;
        // Optional sampling never backpressures uploads. A Copy pair stays reserved until an
        // existing Graphics submission resolves it; later batches may therefore be unsampled.
        std::uint64_t gpu_samples_skipped = 0;
    };

    struct ImageTransferRegion final
    {
        std::uint64_t source_offset = 0, source_row_pitch = 0;
        std::uint32_t block_extent = 1, block_bytes = 4;
        nri::TextureRegionDesc texture{};
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
        struct BufferCopy final
        {
            nri::Buffer* source;
            nri::Buffer* destination;
            std::uint64_t source_offset, destination_offset, bytes;
        };

        void recordCopies(nri::CommandBuffer&, std::span<const BufferCopy>) noexcept;

        using Result = RenderResult<void>;
        using Task = exec::task<Result>;

        ~ResourceUploader() noexcept;
        ResourceUploader(const ResourceUploader&) = delete;
        ResourceUploader& operator=(const ResourceUploader&) = delete;

        // Success means all required Copy/Acquire submissions were accepted. The caller retains targets or retires
        // failed candidates; success does not wait for the final fence observation.
        [[nodiscard]] static RenderResult<std::unique_ptr<ResourceUploader>> create(
            DeviceSession&,
            const TransferOptions&,
            execution::CpuScheduler
        ) noexcept;

        [[nodiscard]] bool reserveSourceBytes(std::size_t bytes) noexcept;
        void releaseSourceBytes(std::size_t bytes) noexcept;

        void setBackgroundTransfersAllowed(bool allowed) noexcept
        {
            background_allowed_ = allowed;
        }

        void reserveFrameStaging(bool reserve) noexcept
        {
            reserve_frame_staging_ = reserve;
        }

        void submitReadyTransfers() noexcept;
        void recordFrameCopies(nri::CommandBuffer&) noexcept;
        void completeFrameCopies(RenderResult<void>) noexcept;
        [[nodiscard]] bool hasFrameCopies() const noexcept;

        [[nodiscard]] Task writeBuffers(
            std::span<const BufferWrite> writes,
            std::span<const BufferZero> fills = {},
            std::uint32_t frame_slot = UINT32_MAX,
            SubmissionFrontiers* progress = nullptr
        ) noexcept;
        // Each source spans the first element through the last element's end, including stride gaps.
        // Pack only element_bytes per element; destination ranges and byte admission use packed sizes.
        // The layout applies to this batch, keeping BufferWrite a compact borrowed range.
        [[nodiscard]] Task writeBuffers(
            std::span<const BufferWrite> writes,
            std::uint32_t element_bytes,
            std::uint32_t source_stride,
            std::uint32_t frame_slot,
            SubmissionFrontiers* progress = nullptr
        ) noexcept;
        [[nodiscard]] Task writeImage(
            nri::Texture& target,
            std::span<const std::byte> source,
            std::vector<ImageTransferRegion> regions,
            const nri::Texture* clone = nullptr
        ) noexcept;

        // Device-thread preparation of already published data. A short result means this finite
        // staging window is full; no Sender or per-frame operation state is created.
        [[nodiscard]] RenderResult<std::size_t> stageBuffer(const BufferWrite&, std::size_t unit = 4) noexcept;

        [[nodiscard]] std::size_t windowRegionLimit() const noexcept
        {
            return options.window_regions;
        }

        [[nodiscard]] std::size_t windowByteLimit() const noexcept
        {
            return options.chunk_bytes * options.staging_pages;
        }

        [[nodiscard]] TransferStats statistics() const noexcept
        {
            return stats;
        }

    private:
        struct Copy;
        struct BatchEntry;
        class BatchSender;
        struct Measurement;
        void enqueue(BatchEntry&) noexcept;
        [[nodiscard]] bool canStage(std::uint32_t frame_slot) const noexcept;
        std::vector<Copy> frame_copies_;
        ResourceUploader(DeviceSession&, TransferOptions, execution::CpuScheduler) noexcept;
        [[nodiscard]] Task write(
            std::span<const BufferWrite>,
            std::span<const BufferZero>,
            nri::Texture*,
            std::span<const std::byte>,
            std::vector<ImageTransferRegion>,
            const nri::Texture*,
            std::uint32_t frame_slot,
            std::uint32_t element_bytes = 0,
            std::uint32_t source_stride = 0,
            SubmissionFrontiers* progress = nullptr
        ) noexcept;
        [[nodiscard]] RenderResult<void> measure(std::size_t page, std::uint32_t bits) noexcept;

        std::unique_ptr<StagingArena> staging_arena_;
        bool background_allowed_ = true;
        bool reserve_frame_staging_ = false;
        DeviceSession& session;
        SubmissionQueue* copy_queue = nullptr;
        TransferOptions options;
        execution::CpuScheduler cpu;
        TransferStats stats;
        BatchEntry* ready_head_ = nullptr;
        BatchEntry* ready_tail_ = nullptr;
        BatchEntry* range_waiters_ = nullptr;
        BatchEntry* range_waiters_tail_ = nullptr;
        std::vector<BatchEntry*> batch_entries_;
        std::unique_ptr<Measurement[]> measurements_;
        std::size_t graphics_measurement_base_ = 0;
        std::uint64_t query_result_stride_ = 16;
        std::array<nri::QueryPool*, 2> timestamps_{};
        nri::Buffer* timestamp_results_ = nullptr;
        std::uint64_t* mapped_timestamps_ = nullptr;
        std::uint32_t copy_timestamp_bits = 0, graphics_timestamp_bits = 0;
    };
} // namespace lux::render::detail
