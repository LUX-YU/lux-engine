#pragma once

#include <lux/engine/render/visibility.h>

#include <exec/task.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/extensions/device/SubmissionQueue.hpp>
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
        struct WriteWaiter final
        {
            WriteWaiter* next = nullptr;
            void* operation = nullptr;
            bool (*stopping)(void*) noexcept = nullptr;
            void (*complete)(void*) noexcept = nullptr;
        };

    public:
        class WriteAdmissionSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(RenderResult<void>)>;

            WriteAdmissionSender(ResourceUploader& uploader, std::size_t bytes) noexcept
                : uploader_(uploader), bytes_(bytes)
            {
            }

            template <class Receiver> struct Operation final
            {
                using operation_state_concept = stdexec::operation_state_t;
                ResourceUploader& uploader;
                Receiver receiver;
                WriteWaiter node;
                std::size_t bytes;
                std::size_t reserved_bytes = 0;

                ~Operation() noexcept
                {
                    uploader.releaseSourceBytes(reserved_bytes);
                }

                void start() & noexcept
                {
                    if (!uploader.reserveSourceBytes(bytes))
                    {
                        stdexec::set_value(
                            std::move(receiver),
                            RenderResult<void>{cxx::unexpected(RenderError{kDeviceCapacity})}
                        );
                        return;
                    }
                    reserved_bytes = bytes;
                    node.operation = this;
                    node.stopping = [](void* value) noexcept
                    {
                        auto& self = *static_cast<Operation*>(value);
                        return stdexec::get_stop_token(stdexec::get_env(self.receiver)).stop_requested();
                    };
                    node.complete = [](void* value) noexcept
                    {
                        auto& self = *static_cast<Operation*>(value);
                        const bool stopped = self.node.stopping(value);
                        auto result = stopped ? RenderResult<void>{cxx::unexpected(RenderError{kUploadStopped})}
                                              : RenderResult<void>{};
                        stdexec::set_value(std::move(self.receiver), std::move(result));
                    };
                    uploader.enqueueWrite(node);
                }
            };

            template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{uploader_, std::move(receiver), {}, bytes_};
            }

        private:
            ResourceUploader& uploader_;
            std::size_t bytes_;
        };

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

        void setBackgroundTransfersAllowed(bool allowed) noexcept
        {
            background_window_ = allowed;
            background_allowed_ = allowed && writes_[write_slot_].pending == 0;
        }

        // The frame owner publishes the sole writable slice; upload operations own only their write borrows.
        void setWriteWindow(std::uint32_t slot, bool writable, bool frozen) noexcept;
        [[nodiscard]] bool tryBeginWrite(std::uint32_t& slot) noexcept;
        void finishWrite(std::uint32_t slot, const SubmissionFrontiers&) noexcept;

        [[nodiscard]] WriteAdmissionSender whenWritable(std::size_t bytes) noexcept
        {
            return {*this, bytes};
        }

        void dispatchWrites() noexcept;

        [[nodiscard]] bool hasWrites(std::uint32_t slot) const noexcept
        {
            return writes_[slot].touched;
        }

        [[nodiscard]] std::uint32_t pendingWrites(std::uint32_t slot) const noexcept
        {
            return writes_[slot].pending;
        }

        [[nodiscard]] bool resourcesFrozen() const noexcept
        {
            return resources_frozen_;
        }

        void finishWrites(std::uint32_t slot, SubmissionFrontiers& accepted) noexcept;

        void reserveFrameStaging(bool reserve) noexcept
        {
            reserve_frame_staging_ = reserve;
        }

        [[nodiscard]] bool canProgressTransfers() const noexcept;
        [[nodiscard]] Task submitReadyTransfers() noexcept;
        [[nodiscard]] Task prepareFrameCopies() noexcept;
        void recordFrameCopies(nri::CommandBuffer&) noexcept;
        // Settles staging after the frame submission attempt; this does not observe GPU completion.
        void finishFrameCopySubmission(RenderResult<void>) noexcept;
        [[nodiscard]] bool hasFrameCopies() const noexcept;

        // Lazy device-thread task. Source ranges and their descriptors stay valid through terminal completion.
        // Success means Copy/Acquire acceptance; it does not wait for GPU completion. Partial failures still
        // require the caller to retain or defer-release targets already referenced by accepted work.
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

        // Reserve an entire write before publication; insufficient finite capacity fails the whole
        // preparation. CPU copying occurs after all source borrows are frozen.
        [[nodiscard]] RenderResult<std::size_t> stageBuffer(const BufferWrite&, std::size_t unit = 4) noexcept;
        // Freeze a transient projection (at most 16 KiB) before its caller's scratch is reused.
        [[nodiscard]] RenderResult<std::size_t> stageInlineBuffer(const BufferWrite&, std::size_t unit = 4) noexcept;

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
        [[nodiscard]] RenderResult<std::size_t> stage(const BufferWrite&, std::size_t, bool copy_now) noexcept;
        struct WriteState final
        {
            SubmissionFrontiers accepted;
            std::uint32_t pending = 0;
            bool touched = false;
        };

        void enqueueWrite(WriteWaiter&) noexcept;
        std::array<WriteState, 3> writes_;
        std::uint32_t write_slot_ = 0;
        bool writes_allowed_ = false, resources_frozen_ = false, background_window_ = true;
        WriteWaiter* frame_waiters_ = nullptr;
        WriteWaiter* frame_waiters_tail_ = nullptr;
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
        bool background_allowed_ = true;
        bool reserve_frame_staging_ = false;
        DeviceSession& device_;
        SubmissionQueue* copy_queue_ = nullptr;
        TransferOptions options_;
        execution::CpuScheduler cpu_scheduler_;
        TransferStats statistics_;
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
        std::uint32_t copy_timestamp_bits_ = 0;
        std::uint32_t graphics_timestamp_bits_ = 0;
    };
} // namespace lux::render::detail
