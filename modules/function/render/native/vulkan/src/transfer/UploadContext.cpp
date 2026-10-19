#include "ImageTransfer.hpp"
#include "StagingArena.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/Error.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

namespace lux::render::vulkan
{
    using UploadClock = std::chrono::steady_clock;

    static std::uint64_t elapsed(UploadClock::time_point start) noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(UploadClock::now() - start).count();
    }

    struct UploadContext::Measurement final
    {
        SubmissionCallback completion;
        UploadContext* owner = nullptr;
        std::size_t index = 0;
        std::uint32_t bits = 0;
    };

    struct UploadContext::Copy final
    {
        StagingRange staging;
        gapi::vk::VmaBuffer* buffer = nullptr;
        gapi::vk::VmaImage* image = nullptr;
        const gapi::vk::VmaImage* clone = nullptr;
        VkDeviceSize offset = 0;
        ImageTransferRegion region;
        std::span<const std::byte> source;
        std::span<const BufferWrite> inputs;
        std::size_t input_offset = 0, bytes = 0;
        std::uint32_t row = 0, rows = 0;
        bool first = false, final = false;
    };

    struct UploadContext::BatchEntry final
    {
        UploadContext* owner;
        SubmissionQueue* queue;
        std::span<Copy> copies;
        std::span<const BufferFill> fills;
        BatchEntry* next = nullptr;
        void* receiver = nullptr;
        void (*complete)(void*, RenderResult<void>) noexcept = nullptr;
        std::uint32_t frame_slot = UINT32_MAX;
        bool (*stopping)(void*) noexcept = nullptr;
        bool acquiring = false;
        bool waiting_range = false;
        std::size_t minimum_range = 4;
    };

    class UploadContext::BatchSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(RenderResult<void>)>;

        explicit BatchSender(BatchEntry& entry) noexcept : entry_(entry) {}

        template <class Receiver> struct Operation final
        {
            using operation_state_concept = stdexec::operation_state_t;
            BatchEntry& entry;
            Receiver receiver;

            void start() & noexcept
            {
                entry.receiver = this;
                entry.stopping = [](void* value) noexcept
                {
                    auto& self = *static_cast<Operation*>(value);
                    return stdexec::get_stop_token(stdexec::get_env(self.receiver)).stop_requested();
                };
                entry.complete = [](void* value, RenderResult<void> result) noexcept
                {
                    auto& self = *static_cast<Operation*>(value);
                    stdexec::set_value(std::move(self.receiver), std::move(result));
                };
                entry.owner->enqueue(entry);
            }
        };

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            return Operation<Receiver>{entry_, std::move(receiver)};
        }

    private:
        BatchEntry& entry_;
    };

    UploadContext::UploadContext(
        DeviceSession& device,
        TransferOptions settings,
        execution::CpuScheduler scheduler
    ) noexcept
        : session(device), options(settings), cpu(std::move(scheduler))
    {
    }

    UploadContext::~UploadContext() noexcept
    {
        if (timestamps)
        {
            vkDestroyQueryPool(session.nativeDevice(), timestamps, nullptr);
        }
    }

    RenderResult<void> DeviceSession::configureUploads(
        const TransferOptions& options,
        execution::CpuScheduler cpu
    ) noexcept
    {
        const bool invalid = uploads_ || cpu == execution::CpuScheduler{} || options.chunk_bytes < 256 ||
                             options.chunk_bytes > 16 * 1024 * 1024 || options.staging_pages == 0 ||
                             options.staging_pages > 16 || options.pending_bytes < options.chunk_bytes ||
                             options.batch_entries == 0 || options.window_regions == 0;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (options.measure_gpu && !enabled_features_.host_query_reset)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        auto state = std::unique_ptr<UploadContext>(new UploadContext(*this, options, cpu));
        const auto& families_info = physical_device_.queueFamilyProperties();
        auto& transfer = queue(EQueueRole::TRANSFER);
        const auto granularity =
            families_info[transfer.nativeQueue().family].queueFamilyProperties.minImageTransferGranularity;
        // This row-chunk algorithm requires independently transferable texel/block rows. Other hardware
        // uses the existing Graphics queue; do not emit invalid sub-image transfers on restrictive families.
        const bool row_chunks = granularity.width == 1 && granularity.height == 1 && granularity.depth == 1;
        state->copy_queue = row_chunks ? &transfer : &queue();
        state->copy_timestamp_bits =
            families_info[state->copy_queue->nativeQueue().family].queueFamilyProperties.timestampValidBits;
        state->graphics_timestamp_bits =
            families_info[queue().nativeQueue().family].queueFamilyProperties.timestampValidBits;
        if (options.measure_gpu && (state->copy_timestamp_bits || state->graphics_timestamp_bits))
        {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            state->graphics_measurement_base_ = state->copy_queue == &queue() ? 0 : state->copy_queue->capacity();
            const auto query_slots = state->graphics_measurement_base_ + queue().capacity();
            state->measurements_ = std::make_unique<UploadContext::Measurement[]>(query_slots);
            for (std::size_t i = 0; i < query_slots; ++i)
            {
                state->measurements_[i].owner = state.get();
                state->measurements_[i].index = i;
            }
            query.queryCount = std::uint32_t(query_slots * 2);
            const auto result = vkCreateQueryPool(nativeDevice(), &query, nullptr, &state->timestamps);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        const std::array families{queue().nativeQueue().family, state->copy_queue->nativeQueue().family};
        const auto sharing = families[0] == families[1] ? std::span<const std::uint32_t>{} : std::span(families);
        auto arena = StagingArena::create(
            allocator_,
            options.chunk_bytes,
            options.staging_pages,
            sharing,
            std::max<VkDeviceSize>(4, physical_device_.properties().properties.limits.nonCoherentAtomSize)
        );
        if (!arena)
        {
            return cxx::unexpected(arena.error());
        }
        staging_arena_ = std::move(*arena);
        state->stats.staging_bytes = options.chunk_bytes * options.staging_pages;
        state->batch_entries_.reserve(options.batch_entries);
        state->frame_copies_.reserve(options.window_regions);
        state->frame_receivers_.reserve(options.window_regions);
        uploads_ = std::move(state);
        return {};
    }

    TransferStats DeviceSession::uploadStats() const noexcept
    {
        return uploads_ ? uploads_->stats : TransferStats{};
    }

    bool UploadContext::reserveBytes(std::size_t bytes) noexcept
    {
        if (bytes > options.pending_bytes - stats.pending_bytes)
        {
            return false;
        }
        stats.pending_bytes += bytes;
        stats.pending_peak = std::max(stats.pending_peak, stats.pending_bytes);
        return true;
    }

    void UploadContext::releaseBytes(std::size_t bytes) noexcept
    {
        stats.pending_bytes -= bytes;
    }

    static void recordBuffer(
        VkCommandBuffer command,
        gapi::vk::VmaBuffer& target,
        StagingRange& range,
        std::size_t offset,
        std::size_t chunk_size,
        SubmissionQueue& copy,
        SubmissionQueue& graphics,
        bool acquiring,
        bool final
    ) noexcept
    {
        const bool distinct = &copy != &graphics;
        const bool cross_family = copy.nativeQueue().family != graphics.nativeQueue().family;
        if (!acquiring)
        {
            const VkBufferCopy copy{range.offset(), offset, chunk_size};
            vkCmdCopyBuffer(command, range.buffer(), target.handle(), 1, &copy);
        }
        if (!final)
        {
            return;
        }
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.buffer = target.handle();
        barrier.size = VK_WHOLE_SIZE;
        barrier.srcStageMask = acquiring ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = acquiring ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = !distinct || acquiring ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = !distinct || acquiring ? VK_ACCESS_2_MEMORY_READ_BIT : VK_ACCESS_2_NONE;
        barrier.srcQueueFamilyIndex = cross_family ? copy.nativeQueue().family : VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = cross_family ? graphics.nativeQueue().family : VK_QUEUE_FAMILY_IGNORED;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    static void recordImage(
        VkCommandBuffer command,
        gapi::vk::VmaImage& target,
        StagingRange& range,
        const ImageTransferRegion& input,
        const gapi::vk::VmaImage* clone,
        std::uint32_t row,
        std::uint32_t rows,
        bool first,
        SubmissionQueue& copy,
        SubmissionQueue& graphics,
        bool acquiring,
        bool final
    ) noexcept
    {
        const bool distinct = &copy != &graphics;
        const bool cross_family = copy.nativeQueue().family != graphics.nativeQueue().family;
        const auto& description = target;
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.image = target.handle();
        barrier
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, description.mipLevels(), 0, description.arrayLayers()};
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        if (!acquiring)
        {
            if (first)
            {
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                vkCmdPipelineBarrier2(command, &dependency);
                if (clone)
                {
                    auto original = barrier;
                    original.image = clone->handle();
                    // The device sampled-image table is visible to every supported shader stage.
                    original.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    original.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    original.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    original.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                    original.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    original.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    dependency.pImageMemoryBarriers = &original;
                    vkCmdPipelineBarrier2(command, &dependency);
                    for (std::uint32_t mip = 0; mip < description.mipLevels(); ++mip)
                    {
                        VkImageCopy copy{};
                        copy.srcSubresource =
                            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, description.arrayLayers()};
                        copy.extent = {
                            std::max(1u, description.extent().width >> mip),
                            std::max(1u, description.extent().height >> mip),
                            1
                        };
                        vkCmdCopyImage(
                            command,
                            original.image,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            barrier.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1,
                            &copy
                        );
                    }
                    original.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    original.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                    original.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    original.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    original.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    original.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    vkCmdPipelineBarrier2(command, &dependency);
                    dependency.pImageMemoryBarriers = &barrier;
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    vkCmdPipelineBarrier2(command, &dependency);
                }
            }
            VkBufferImageCopy region{};
            region.bufferOffset = range.offset();
            region.imageSubresource = input.subresource;
            region.imageOffset = input.offset;
            region.imageOffset.y += std::int32_t(row * input.block_extent);
            region.imageExtent = {
                input.extent.width,
                std::uint32_t(std::min(
                    std::uint64_t(rows) * input.block_extent,
                    std::uint64_t(input.extent.height - row * input.block_extent)
                )),
                1
            };
            vkCmdCopyBufferToImage(
                command,
                range.buffer(),
                barrier.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &region
            );
        }
        if (final)
        {
            barrier.srcStageMask = acquiring ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = acquiring ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask =
                !distinct || acquiring ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = !distinct || acquiring ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE;
            barrier.oldLayout = acquiring && !cross_family ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                           : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (cross_family)
            {
                barrier.srcQueueFamilyIndex = copy.nativeQueue().family;
                barrier.dstQueueFamilyIndex = graphics.nativeQueue().family;
            }
            vkCmdPipelineBarrier2(command, &dependency);
        }
    }

    RenderResult<void> UploadContext::measure(std::size_t page, std::uint32_t bits) noexcept
    {
        std::array<std::uint64_t, 2> values{};
        const auto result = vkGetQueryPoolResults(
            session.nativeDevice(),
            timestamps,
            std::uint32_t(page * 2),
            2,
            sizeof(values),
            values.data(),
            sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT
        );
        if (result != VK_SUCCESS)
        {
            ++stats.gpu_query_errors;
            return cxx::unexpected(nativeError(result));
        }
        const auto mask = bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
        stats.gpu_ns += std::uint64_t(
            double((values[1] - values[0]) & mask) *
            session.physicalDevice().properties().properties.limits.timestampPeriod
        );
        ++stats.gpu_samples;
        return {};
    }

    RenderResult<std::size_t> UploadContext::prepareBuffer(const BufferWrite& write, std::size_t unit) noexcept
    {
        if (frame_copies_.size() == options.window_regions)
        {
            return 0;
        }
        auto range = session.staging_arena_->acquire(std::min(write.source.size(), options.chunk_bytes), unit);
        if (!range)
        {
            return 0;
        }
        const auto bytes = range->bytes().size();
        std::memcpy(range->bytes().data(), write.source.data(), bytes);
        if (auto result = range->flush(); result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        Copy copy{std::move(*range)};
        copy.buffer = write.target;
        copy.offset = write.offset;
        copy.bytes = bytes;
        copy.final = true;
        frame_copies_.push_back(std::move(copy));
        stats.copied_bytes += bytes;
        return bytes;
    }

    void UploadContext::afterFrameSubmit(void* receiver, void (*complete)(void*, RenderResult<void>) noexcept) noexcept
    {
        frame_receivers_.emplace_back(receiver, complete);
    }

    bool UploadContext::hasFrameCopies() const noexcept
    {
        return !frame_copies_.empty();
    }

    void UploadContext::recordFrame(CommandBatch& batch) noexcept
    {
        if (frame_copies_.empty())
        {
            return;
        }
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(batch.native(), &dependency);
        for (auto& copy : frame_copies_)
        {
            recordBuffer(
                batch.native(),
                *copy.buffer,
                copy.staging,
                copy.offset,
                copy.bytes,
                session.queue(),
                session.queue(),
                false,
                false
            );
        }
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        vkCmdPipelineBarrier2(batch.native(), &dependency);
    }

    void UploadContext::finishFrameCopies(RenderResult<void> result) noexcept
    {
        if (result)
        {
            for (auto& copy : frame_copies_)
            {
                copy.staging.commit(session.queue());
                ++stats.copy_regions;
            }
        }
        frame_copies_.clear();
        for (auto [receiver, complete] : frame_receivers_)
        {
            complete(receiver, result);
        }
        frame_receivers_.clear();
    }

    void UploadContext::enqueueFrame(FrameWriteSender::Node& node) noexcept
    {
        node.next = nullptr;
        if (frame_waiters_tail_)
        {
            frame_waiters_tail_->next = &node;
        }
        else
        {
            frame_waiters_ = &node;
        }
        frame_waiters_tail_ = &node;
    }

    bool UploadContext::canStage(std::uint32_t frame_slot) const noexcept
    {
        if (frame_slot != UINT32_MAX)
        {
            return true;
        }
        const auto& frame = session.frames_[session.next_frame_];
        return frame.phase != DeviceSession::EFramePhase::PREPARING && frame.pending_writes == 0;
    }

    void UploadContext::enqueue(BatchEntry& entry) noexcept
    {
        // Current-frame writers precede background range waiters in the same finite list.
        // The active render workload reserves one existing page for frame preparation; an
        // upload-only workload can use every page. Both use the same Arena and waiter list.
        if (entry.waiting_range && entry.frame_slot != UINT32_MAX)
        {
            entry.next = range_waiters_;
            range_waiters_ = &entry;
            if (!range_waiters_tail_)
            {
                range_waiters_tail_ = &entry;
            }
            return;
        }
        auto*& head = entry.waiting_range ? range_waiters_ : ready_head_;
        auto*& tail = entry.waiting_range ? range_waiters_tail_ : ready_tail_;
        entry.next = nullptr;
        if (tail)
        {
            tail->next = &entry;
        }
        else
        {
            head = &entry;
        }
        tail = &entry;
    }

    void UploadContext::flush() noexcept
    {
        // A bounded prefix only. Completing a Sender may enqueue later work, never extend this flush.
        auto* boundary = ready_tail_;
        BatchEntry* blocked_head = nullptr;
        BatchEntry* blocked_tail = nullptr;
        while (ready_head_ && boundary)
        {
            auto& first = *ready_head_;
            auto& queue = first.acquiring ? session.queue() : *first.queue;
            auto batch = queue.deviceLost()
                             ? RenderResult<CommandBatch>{cxx::unexpected(nativeError(VK_ERROR_DEVICE_LOST))}
                             : queue.beginAfterPoll();
            if (!batch && batch.error().type == kCapacity)
            {
                // Temporarily detach blocked groups from this flush's original prefix.
                // Restore them ahead of newly enqueued work, preserving per-Queue FIFO even
                // when groups for the same Queue were separated by another Queue's entries.
                auto* begin = ready_head_;
                auto* end = begin;
                while (end != boundary && end->next && end->next->queue == begin->queue &&
                       end->next->acquiring == begin->acquiring)
                {
                    end = end->next;
                }
                ready_head_ = end->next;
                if (!ready_head_)
                {
                    ready_tail_ = nullptr;
                }
                end->next = nullptr;
                if (blocked_tail)
                {
                    blocked_tail->next = begin;
                }
                else
                {
                    blocked_head = begin;
                }
                blocked_tail = end;
                if (end == boundary)
                {
                    boundary = nullptr;
                }
                continue;
            }
            batch_entries_.clear();
            const bool acquiring = first.acquiring;
            const auto* source_queue = first.queue;
            while (ready_head_ && batch_entries_.size() < batch_entries_.capacity())
            {
                auto* entry = ready_head_;
                const bool same_submission = entry->queue == source_queue && entry->acquiring == acquiring;
                if (!same_submission)
                {
                    break;
                }
                ready_head_ = entry->next;
                entry->next = nullptr;
                batch_entries_.push_back(entry);
                if (entry == boundary)
                {
                    boundary = nullptr;
                    break;
                }
            }
            if (!ready_head_)
            {
                ready_tail_ = nullptr;
            }
            RenderResult<void> accepted;
            if (!batch)
            {
                accepted = cxx::unexpected(batch.error());
            }
            else
            {
                const auto started = UploadClock::now();
                const auto bits = &queue == &session.queue() ? graphics_timestamp_bits : copy_timestamp_bits;
                const auto query_index = (&queue == &session.queue() ? graphics_measurement_base_ : 0) + batch->slot();
                const bool timestamp = timestamps && bits && !acquiring;
                if (timestamp)
                {
                    vkResetQueryPool(session.nativeDevice(), timestamps, std::uint32_t(query_index * 2), 2);
                    vkCmdWriteTimestamp2(
                        batch->native(),
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                        timestamps,
                        std::uint32_t(query_index * 2)
                    );
                }
                for (auto* entry : batch_entries_)
                {
                    const auto bufferBarrier = [&](VkPipelineStageFlags2 source_stage,
                                                   VkAccessFlags2 source_access,
                                                   VkPipelineStageFlags2 target_stage,
                                                   VkAccessFlags2 target_access)
                    {
                        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                        barrier.srcStageMask = source_stage;
                        barrier.srcAccessMask = source_access;
                        barrier.dstStageMask = target_stage;
                        barrier.dstAccessMask = target_access;
                        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        dependency.memoryBarrierCount = 1;
                        dependency.pMemoryBarriers = &barrier;
                        vkCmdPipelineBarrier2(batch->native(), &dependency);
                    };
                    if (!entry->fills.empty())
                    {
                        bufferBarrier(
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT
                        );
                        for (const auto& fill : entry->fills)
                        {
                            vkCmdFillBuffer(batch->native(), fill.target->handle(), fill.offset, fill.size, fill.value);
                        }
                        if (!entry->fills.empty())
                        {
                            bufferBarrier(
                                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT
                            );
                        }
                        bufferBarrier(
                            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
                        );
                    }
                    for (auto& copy : entry->copies)
                    {
                        if (acquiring && !copy.final)
                        {
                            continue;
                        }
                        if (copy.buffer)
                        {
                            // Existing arena/slice ranges use Graphics; queue order and this dependency
                            // cover prior reads without a Host fence wait.
                            if (!acquiring)
                            {
                                VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                                barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                                barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                barrier.buffer = copy.buffer->handle();
                                barrier.offset = copy.offset;
                                barrier.size = copy.bytes;
                                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                                dependency.bufferMemoryBarrierCount = 1;
                                dependency.pBufferMemoryBarriers = &barrier;
                                vkCmdPipelineBarrier2(batch->native(), &dependency);
                            }
                            recordBuffer(
                                batch->native(),
                                *copy.buffer,
                                copy.staging,
                                copy.offset,
                                copy.bytes,
                                queue,
                                session.queue(),
                                acquiring,
                                copy.final
                            );
                        }
                        else
                        {
                            recordImage(
                                batch->native(),
                                *copy.image,
                                copy.staging,
                                copy.region,
                                copy.clone,
                                copy.row,
                                copy.rows,
                                copy.first,
                                *entry->queue,
                                session.queue(),
                                acquiring,
                                copy.final
                            );
                        }
                    }
                }
                if (timestamp)
                {
                    vkCmdWriteTimestamp2(
                        batch->native(),
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        timestamps,
                        std::uint32_t(query_index * 2 + 1)
                    );
                }
                accepted = acquiring
                               ? std::move(*batch).submitAfter(*source_queue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)
                               : std::move(*batch).submit();
                stats.owner_ns += elapsed(started);
                if (accepted)
                {
                    acquiring ? ++stats.acquire_submissions : ++stats.copy_submissions;
                }
                if (accepted)
                {
                    for (auto* entry : batch_entries_)
                    {
                        if (entry->frame_slot != UINT32_MAX)
                        {
                            session.frames_[entry->frame_slot].accepted_serials[session.queue_roles_[0]] =
                                queue.submitted();
                        }
                    }
                }
                if (accepted && timestamp)
                {
                    auto& measurement = measurements_[query_index];
                    measurement.bits = bits;
                    queue.arm(
                        queue.submitted(),
                        measurement.completion,
                        [](void* value, RenderResult<void> completed) noexcept
                        {
                            auto& state = *static_cast<Measurement*>(value);
                            if (completed)
                            {
                                (void)state.owner->measure(state.index, state.bits);
                            }
                        },
                        &measurement
                    );
                }
                if (accepted && !acquiring)
                {
                    for (auto* entry : batch_entries_)
                    {
                        for (auto& copy : entry->copies)
                        {
                            copy.staging.commit(queue);
                            ++stats.copy_regions;
                        }
                    }
                }
            }
            for (auto* entry : batch_entries_)
            {
                const bool needs_acquire =
                    accepted && !acquiring && entry->queue != &session.queue() &&
                    std::ranges::any_of(entry->copies, [](const auto& copy) { return copy.final; });
                if (needs_acquire)
                {
                    entry->acquiring = true;
                    enqueue(*entry);
                }
                else
                {
                    const auto complete = entry->complete;
                    auto* receiver = entry->receiver;
                    complete(receiver, accepted);
                    // The operation and its borrowed entry may be gone.
                }
            }
        }
        if (blocked_head)
        {
            blocked_tail->next = ready_head_;
            ready_head_ = blocked_head;
            if (!ready_tail_)
            {
                ready_tail_ = blocked_tail;
            }
        }
        const auto phase = session.frames_[session.next_frame_].phase;
        const bool can_write =
            phase == DeviceSession::EFramePhase::FREE || phase == DeviceSession::EFramePhase::COLLECTING;
        auto* frame_boundary = frame_waiters_tail_;
        while (frame_waiters_ && frame_boundary)
        {
            auto* node = frame_waiters_;
            if (!can_write && !node->stopping(node->operation))
            {
                break;
            }
            frame_waiters_ = node->next;
            if (!frame_waiters_)
            {
                frame_waiters_tail_ = nullptr;
            }
            if (node == frame_boundary)
            {
                frame_boundary = nullptr;
            }
            const auto complete = node->complete;
            auto* operation = node->operation;
            complete(operation);
        }
        auto* range_boundary = range_waiters_tail_;
        while (range_waiters_ && range_boundary)
        {
            auto* entry = range_waiters_;
            const bool stopping = entry->stopping(entry->receiver);
            if (!stopping && !session.deviceLost() &&
                (!canStage(entry->frame_slot) || !session.staging_arena_->canAcquire(
                                                     entry->minimum_range,
                                                     entry->frame_slot == UINT32_MAX && session.rendering_
                                                 )))
            {
                break;
            }
            range_waiters_ = entry->next;
            if (!range_waiters_)
            {
                range_waiters_tail_ = nullptr;
            }
            if (entry == range_boundary)
            {
                range_boundary = nullptr;
            }
            entry->next = nullptr;
            const auto complete = entry->complete;
            auto* receiver = entry->receiver;
            complete(
                receiver,
                session.deviceLost() ? RenderResult<void>{cxx::unexpected(nativeError(VK_ERROR_DEVICE_LOST))}
                : stopping           ? RenderResult<void>{cxx::unexpected(RenderError{kWorkStopped})}
                                     : RenderResult<void>{}
            );
        }
    }

    UploadContext::Task UploadContext::writeBuffers(
        std::span<const BufferWrite> writes,
        std::span<const BufferFill> fills,
        std::uint32_t frame_slot
    ) noexcept
    {
        return write(writes, fills, nullptr, {}, {}, nullptr, frame_slot);
    }

    UploadContext::Task UploadContext::writeImage(
        gapi::vk::VmaImage& target,
        std::span<const std::byte> source,
        std::vector<ImageTransferRegion> regions,
        const gapi::vk::VmaImage* clone
    ) noexcept
    {
        return write({}, {}, &target, source, std::move(regions), clone, UINT32_MAX);
    }

    UploadContext::Task UploadContext::writeBuffers(
        std::span<const BufferWrite> writes,
        std::uint32_t element_bytes,
        std::uint32_t source_stride,
        std::uint32_t frame_slot
    ) noexcept
    {
        return write(writes, {}, nullptr, {}, {}, nullptr, frame_slot, element_bytes, source_stride);
    }

    UploadContext::Task UploadContext::write(
        std::span<const BufferWrite> writes,
        std::span<const BufferFill> fills,
        gapi::vk::VmaImage* image,
        std::span<const std::byte> source,
        std::vector<ImageTransferRegion> regions,
        const gapi::vk::VmaImage* clone,
        std::uint32_t frame_slot,
        std::uint32_t element_bytes,
        std::uint32_t source_stride
    ) noexcept
    {
        const bool strided = element_bytes != 0 || source_stride != 0;
        const bool invalid_layout = strided && (!element_bytes || element_bytes % 4 || source_stride < element_bytes);
        if (invalid_layout)
        {
            co_return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto buffer_size = [=](const BufferWrite& input) noexcept
        {
            return strided ? ((input.source.size() - element_bytes) / source_stride + 1) * element_bytes
                           : input.source.size();
        };
        std::size_t pending_bytes = 0;
        std::size_t buffer_runs = 0;
        const gapi::vk::VmaBuffer* previous_target = nullptr;
        VkDeviceSize previous_end = 0;
        if (!image)
        {
            for (const auto& input : writes)
            {
                if (!input.target)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const bool invalid_source = strided ? input.source.size() < element_bytes ||
                                                          (input.source.size() - element_bytes) % source_stride != 0
                                                    : input.source.empty();
                if (invalid_source)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const auto bytes = buffer_size(input);
                const bool invalid = bytes % 4 || input.offset % 4 || input.offset > input.target->size() ||
                                     bytes > input.target->size() - input.offset ||
                                     !(input.target->usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                if (invalid)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
                if (bytes > options.pending_bytes - pending_bytes)
                {
                    co_return cxx::unexpected(RenderError{kCapacity});
                }
                pending_bytes += bytes;
                buffer_runs += input.target != previous_target || input.offset != previous_end;
                previous_target = input.target;
                previous_end = input.offset + bytes;
            }
        }
        else
        {
            if (!image || source.empty() || regions.empty())
            {
                co_return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const bool invalid =
                image->samples() != VK_SAMPLE_COUNT_1_BIT || !(image->usage() & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (invalid)
            {
                co_return cxx::unexpected(RenderError{kInvalidArgument});
            }
            for (const auto& region : regions)
            {
                if (auto valid = imageRegionBytes(
                        image->extent(),
                        image->mipLevels(),
                        image->arrayLayers(),
                        region,
                        source.size(),
                        options.chunk_bytes
                    );
                    !valid)
                {
                    co_return cxx::unexpected(valid.error());
                }
            }
            pending_bytes = source.size();
        }
        for (const auto& fill : fills)
        {
            if (!fill.target)
            {
                co_return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const bool invalid = !fill.size || (fill.offset | fill.size) % 4 || fill.offset > fill.target->size() ||
                                 fill.size > fill.target->size() - fill.offset ||
                                 !(fill.target->usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            if (invalid)
            {
                co_return cxx::unexpected(RenderError{kInvalidArgument});
            }
        }
        if (!image && writes.empty() && fills.empty())
        {
            co_return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (!reserveBytes(pending_bytes))
        {
            co_return cxx::unexpected(RenderError{kCapacity});
        }
        cxx::scope_exit budget([&]() noexcept { releaseBytes(pending_bytes); });
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        auto& copy_queue = !image || clone ? session.queue() : *this->copy_queue;
        bool accepted_any = false;
        const auto failure = [&](RenderError error) noexcept
        {
            const bool lost =
                error.type == kNativeFailure && error.args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST);
            return cxx::unexpected(accepted_any && lost ? RenderError{kOutcomeUnknown} : error);
        };
        std::vector<Copy> copies;
        const auto ranges = image ? regions.size() : buffer_runs;
        copies.reserve(std::min(options.window_regions, std::max(options.staging_pages, ranges)));
        std::size_t index = 0, offset = 0, run_end = 0, run_remaining = 0;
        std::uint32_t row = 0;
        const auto count = image ? regions.size() : writes.size();
        while (index < count || !accepted_any)
        {
            if (stop.stop_requested())
            {
                co_return failure(RenderError{kWorkStopped});
            }
            copies.clear();
            while (index < count && copies.size() < copies.capacity() && canStage(frame_slot))
            {
                const auto* region = image ? &regions[index] : nullptr;
                const auto row_bytes =
                    region ? std::size_t((region->extent.width - 1) / region->block_extent + 1) * region->block_bytes
                           : 0;
                if (!region && run_remaining == 0)
                {
                    run_end = index + 1;
                    run_remaining = buffer_size(writes[index]) - offset;
                    auto end_offset = writes[index].offset + buffer_size(writes[index]);
                    while (run_end < count && writes[run_end].target == writes[index].target &&
                           writes[run_end].offset == end_offset)
                    {
                        const auto bytes = buffer_size(writes[run_end]);
                        run_remaining += bytes;
                        end_offset += bytes;
                        ++run_end;
                    }
                }
                const auto remaining =
                    region ? std::size_t((region->extent.height - 1) / region->block_extent + 1 - row) * row_bytes
                           : run_remaining;
                auto range = session.staging_arena_->acquire(
                    std::min(remaining, options.chunk_bytes),
                    region ? row_bytes : 4,
                    frame_slot == UINT32_MAX && session.rendering_
                );
                if (!range)
                {
                    break;
                }
                const auto bytes = range->bytes().size();
                Copy chunk{std::move(*range)};
                chunk.bytes = bytes;
                chunk.buffer = region ? nullptr : writes[index].target;
                chunk.offset = region ? 0 : writes[index].offset + offset;
                chunk.image = image;
                chunk.clone = clone;
                chunk.first = !accepted_any && copies.empty();
                if (region)
                {
                    chunk.region = *region;
                    chunk.source = source;
                    chunk.row = row;
                    chunk.rows = static_cast<std::uint32_t>(bytes / row_bytes);
                    chunk.final = bytes == remaining && index + 1 == count;
                    row += chunk.rows;
                    if (bytes == remaining)
                    {
                        ++index;
                        row = 0;
                    }
                }
                else
                {
                    const auto first_input = index;
                    chunk.input_offset = offset;
                    auto consumed = bytes;
                    while (consumed)
                    {
                        const auto input_bytes = buffer_size(writes[index]);
                        const auto part = std::min(consumed, input_bytes - offset);
                        consumed -= part;
                        offset += part;
                        if (offset == input_bytes)
                        {
                            ++index;
                            offset = 0;
                        }
                    }
                    chunk.inputs = writes.subspan(first_input, index - first_input + (offset != 0));
                    chunk.final = true;
                    run_remaining -= bytes;
                }
                copies.push_back(std::move(chunk));
            }
            if (copies.empty() && index < count)
            {
                BatchEntry wait{this, &copy_queue, {}};
                wait.waiting_range = true;
                wait.frame_slot = frame_slot;
                if (image)
                {
                    const auto& region = regions[index];
                    wait.minimum_range =
                        std::size_t((region.extent.width - 1) / region.block_extent + 1) * region.block_bytes;
                }
                auto available = co_await BatchSender{wait};
                if (!available)
                {
                    co_return failure(available.error());
                }
                continue;
            }
            if (!copies.empty())
            {
                ++stats.prepare_windows;
                auto prepared = co_await (
                    stdexec::schedule(cpu) |
                    stdexec::then(
                        [&]() noexcept
                        {
                            const auto started = UploadClock::now();
                            for (auto& copy : copies)
                            {
                                auto target = copy.staging.bytes();
                                if (copy.buffer)
                                {
                                    auto offset = copy.input_offset;
                                    std::size_t packed = 0;
                                    for (const auto& input : copy.inputs)
                                    {
                                        const auto count =
                                            std::min(target.size() - packed, buffer_size(input) - offset);
                                        if (!strided || source_stride == element_bytes)
                                        {
                                            std::memcpy(target.data() + packed, input.source.data() + offset, count);
                                        }
                                        else
                                        {
                                            auto local = offset % element_bytes;
                                            auto source_offset = (offset / element_bytes) * source_stride + local;
                                            auto* destination = target.data() + packed;
                                            auto remaining = count;
                                            while (remaining)
                                            {
                                                const auto bytes = std::min(remaining, element_bytes - local);
                                                std::memcpy(destination, input.source.data() + source_offset, bytes);
                                                destination += bytes;
                                                source_offset += source_stride - local;
                                                remaining -= bytes;
                                                local = 0;
                                            }
                                        }
                                        packed += count;
                                        offset = 0;
                                    }
                                }
                                else
                                {
                                    const auto row_bytes = target.size() / copy.rows;
                                    const auto* input = copy.source.data() + copy.region.source_offset +
                                                        copy.row * copy.region.source_row_pitch;
                                    for (std::uint32_t r = 0; r < copy.rows; ++r)
                                    {
                                        std::memcpy(
                                            target.data() + r * row_bytes,
                                            input + r * copy.region.source_row_pitch,
                                            row_bytes
                                        );
                                    }
                                }
                            }
                            return RenderResult<std::uint64_t>{elapsed(started)};
                        }
                    ) |
                    stdexec::upon_error(
                        [](execution::EExecutionError error) noexcept
                        {
                            return RenderResult<std::uint64_t>{
                                cxx::unexpected(RenderError{kCpuScheduleFailure, {std::uint64_t(error)}})
                            };
                        }
                    ) |
                    stdexec::upon_stopped(
                        []() noexcept { return RenderResult<std::uint64_t>{cxx::unexpected(RenderError{kCpuStopped})}; }
                    )
                );
                if (!prepared)
                {
                    co_return failure(prepared.error());
                }
                stats.prepare_ns += *prepared;
                for (auto& copy : copies)
                {
                    stats.copied_bytes += copy.staging.bytes().size();
                    if (auto flushed = copy.staging.flush(); flushed != VK_SUCCESS)
                    {
                        co_return failure(nativeError(flushed));
                    }
                }
            }
            if (stop.stop_requested())
            {
                co_return failure(RenderError{kWorkStopped});
            }
            BatchEntry entry{this, &copy_queue, copies};
            entry.frame_slot = frame_slot;
            if (!accepted_any)
            {
                entry.fills = fills;
            }
            auto submitted = co_await BatchSender{entry};
            if (!submitted)
            {
                // An Acquire failure can follow accepted Copy. Targets stay in the professional
                // candidate and the existing Session cleanup covers every accepted queue.
                accepted_any |= entry.acquiring;
                co_return failure(submitted.error());
            }
            accepted_any = true;
        }
        co_return RenderResult<void>{};
    }

} // namespace lux::render::vulkan
