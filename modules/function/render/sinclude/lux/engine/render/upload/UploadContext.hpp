#pragma once

#include <lux/engine/render/visibility.h>

#include <exec/task.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/device/Device.hpp>
#include <lux/engine/render/device/Submission.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <span>

namespace lux::render::detail
{
    class DeviceSession;
    class StagingRange;

    struct TransferStats final
    {
        std::uint64_t copied_bytes = 0, copy_regions = 0, prepare_ns = 0, record_submit_ns = 0;
        std::uint64_t prepare_windows = 0, copy_submissions = 0, acquire_submissions = 0;
        std::uint64_t staging_bytes = 0, pending_bytes = 0, pending_peak = 0;
        std::uint64_t gpu_ns = 0, gpu_samples = 0, gpu_query_errors = 0;
    };

    struct ImageTransferRegion final
    {
        std::uint64_t source_offset = 0, source_row_pitch = 0;
        std::uint32_t block_extent = 1, block_bytes = 4;
        VkImageSubresourceLayers subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        VkOffset3D offset{};
        VkExtent3D extent{};
    };

    struct BufferWrite final
    {
        gapi::vk::VmaBuffer* target = nullptr;
        VkDeviceSize offset = 0;
        std::span<const std::byte> source;
    };

    struct BufferFill final
    {
        gapi::vk::VmaBuffer* target = nullptr;
        VkDeviceSize offset = 0, size = 0;
        std::uint32_t value = 0;
    };

    // One device's upload facilities. Tasks borrow their source and operation-owned targets; this object
    // owns neither professional candidates nor running tasks. Start with the RenderThread scheduler.
    class LUX_RENDER_PUBLIC UploadContext final
    {
        class FrameWriteSender final
        {
        public:
            struct Node final
            {
                Node* next = nullptr;
                void* operation = nullptr;
                bool (*stopping)(void*) noexcept = nullptr;
                void (*complete)(void*) noexcept = nullptr;
            };

            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(RenderResult<void>)>;

            FrameWriteSender(UploadContext& uploads, std::size_t bytes) noexcept : uploads_(uploads), bytes_(bytes) {}

            template <class Receiver> struct Operation final
            {
                using operation_state_concept = stdexec::operation_state_t;
                UploadContext& uploads;
                Receiver receiver;
                Node node;
                std::size_t bytes;
                std::size_t reserved_bytes = 0;

                ~Operation() noexcept
                {
                    uploads.releaseBytes(reserved_bytes);
                }

                void start() & noexcept
                {
                    if (!uploads.reserveBytes(bytes))
                    {
                        stdexec::set_value(
                            std::move(receiver),
                            RenderResult<void>{cxx::unexpected(RenderError{kCapacity})}
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
                        auto result = stopped ? RenderResult<void>{cxx::unexpected(RenderError{kWorkStopped})}
                                              : RenderResult<void>{};
                        stdexec::set_value(std::move(self.receiver), std::move(result));
                    };
                    uploads.enqueueFrame(node);
                }
            };

            template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{uploads_, std::move(receiver), {}, bytes_};
            }

        private:
            UploadContext& uploads_;
            std::size_t bytes_;
        };

    public:
        using Result = RenderResult<void>;
        using Task = exec::task<Result>;

        ~UploadContext() noexcept;
        UploadContext(const UploadContext&) = delete;
        UploadContext& operator=(const UploadContext&) = delete;

        // Success means all required Copy/Acquire submissions were accepted. The caller retains targets or retires
        // failed candidates; success does not wait for the final fence observation.
        [[nodiscard]] FrameWriteSender whenFrameWritable(std::size_t source_bytes) noexcept
        {
            return FrameWriteSender{*this, source_bytes};
        }

        [[nodiscard]] bool tryBeginFrameWrite(std::uint32_t& slot) noexcept;
        void finishFrameWrite(std::uint32_t slot) noexcept;

        [[nodiscard]] Task writeBuffers(
            std::span<const BufferWrite> writes,
            std::span<const BufferFill> fills = {},
            std::uint32_t frame_slot = UINT32_MAX
        ) noexcept;
        // Each source spans the first element through the last element's end, including stride gaps.
        // Pack only element_bytes per element; destination ranges and byte admission use packed sizes.
        // The layout applies to this batch, keeping BufferWrite a compact borrowed range.
        [[nodiscard]] Task writeBuffers(
            std::span<const BufferWrite> writes,
            std::uint32_t element_bytes,
            std::uint32_t source_stride,
            std::uint32_t frame_slot
        ) noexcept;
        [[nodiscard]] Task writeImage(
            gapi::vk::VmaImage& target,
            std::span<const std::byte> source,
            std::vector<ImageTransferRegion> regions,
            const gapi::vk::VmaImage* clone = nullptr
        ) noexcept;

        // Owner-only preparation of already published data. A short result means this finite
        // staging window is full; no Sender or per-frame operation state is created.
        [[nodiscard]] RenderResult<std::size_t> prepareBuffer(const BufferWrite&, std::size_t unit = 4) noexcept;
        // Contributed work only. The borrowed receiver survives this synchronous submission segment;
        // the callback acknowledges acceptance, never GPU completion or physical cleanup.
        void afterFrameSubmit(void* receiver, void (*complete)(void*, RenderResult<void>) noexcept) noexcept;

        [[nodiscard]] std::size_t windowRegionLimit() const noexcept
        {
            return options.window_regions;
        }

        [[nodiscard]] TransferStats statistics() const noexcept
        {
            return stats;
        }

    private:
        friend class DeviceSession;
        struct Copy;
        struct BatchEntry;
        class BatchSender;
        struct Measurement;
        void enqueue(BatchEntry&) noexcept;
        [[nodiscard]] bool canStage(std::uint32_t frame_slot) const noexcept;
        void enqueueFrame(FrameWriteSender::Node&) noexcept;
        [[nodiscard]] bool reserveBytes(std::size_t bytes) noexcept;
        void releaseBytes(std::size_t bytes) noexcept;
        void flush() noexcept;
        void recordFrame(CommandBatch&) noexcept;
        void finishFrameCopies(RenderResult<void>) noexcept;
        [[nodiscard]] bool hasFrameCopies() const noexcept;
        std::vector<Copy> frame_copies_;
        std::vector<std::pair<void*, void (*)(void*, Result) noexcept>> frame_receivers_;
        UploadContext(DeviceSession&, TransferOptions, execution::CpuScheduler) noexcept;
        [[nodiscard]] Task write(
            std::span<const BufferWrite>,
            std::span<const BufferFill>,
            gapi::vk::VmaImage*,
            std::span<const std::byte>,
            std::vector<ImageTransferRegion>,
            const gapi::vk::VmaImage*,
            std::uint32_t frame_slot,
            std::uint32_t element_bytes = 0,
            std::uint32_t source_stride = 0
        ) noexcept;
        [[nodiscard]] RenderResult<void> measure(std::size_t page, std::uint32_t bits) noexcept;

        DeviceSession& session;
        SubmissionQueue* copy_queue = nullptr;
        TransferOptions options;
        execution::CpuScheduler cpu;
        TransferStats stats;
        FrameWriteSender::Node* frame_waiters_ = nullptr;
        FrameWriteSender::Node* frame_waiters_tail_ = nullptr;
        BatchEntry* ready_head_ = nullptr;
        BatchEntry* ready_tail_ = nullptr;
        BatchEntry* range_waiters_ = nullptr;
        BatchEntry* range_waiters_tail_ = nullptr;
        std::vector<BatchEntry*> batch_entries_;
        std::unique_ptr<Measurement[]> measurements_;
        std::size_t graphics_measurement_base_ = 0;
        VkQueryPool timestamps = VK_NULL_HANDLE;
        std::uint32_t copy_timestamp_bits = 0, graphics_timestamp_bits = 0;
    };
} // namespace lux::render::detail
