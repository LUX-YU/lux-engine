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

    struct UploadContext::BatchEntry final
    {
        struct Copy final
        {
            StagingRange staging;
            gapi::vk::VmaBuffer* buffer = nullptr;
            gapi::vk::VmaImage* image = nullptr;
            const gapi::vk::VmaImage* clone = nullptr;
            VkDeviceSize offset = 0;
            ImageTransferRegion region;
            std::span<const std::byte> source;
            std::uint32_t row = 0, rows = 0;
            bool first = false, final = false;
        };

        UploadContext* owner;
        SubmissionQueue* queue;
        std::span<Copy> copies;
        std::span<const BufferCopy> gpu_copies;
        std::span<const BufferFill> fills;
        std::span<const BufferWrite> updates;
        BatchEntry* next = nullptr;
        void* receiver = nullptr;
        void (*complete)(void*, RenderResult<void>) noexcept = nullptr;
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
                             options.batch_entries == 0;
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

    void UploadContext::enqueue(BatchEntry& entry) noexcept
    {
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
        while (ready_head_ && boundary)
        {
            auto& first = *ready_head_;
            auto& queue = first.acquiring ? session.queue() : *first.queue;
            auto batch = queue.deviceLost()
                             ? RenderResult<CommandBatch>{cxx::unexpected(nativeError(VK_ERROR_DEVICE_LOST))}
                             : queue.beginAfterPoll();
            if (!batch && batch.error().type == kCapacity)
            {
                break;
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
                    if (!entry->fills.empty() || !entry->gpu_copies.empty() || !entry->updates.empty())
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
                        for (const auto& copy : entry->gpu_copies)
                        {
                            vkCmdCopyBuffer(
                                batch->native(),
                                copy.source->handle(),
                                copy.target->handle(),
                                1,
                                &copy.range
                            );
                        }
                        for (const auto& write : entry->updates)
                        {
                            vkCmdUpdateBuffer(
                                batch->native(),
                                write.target->handle(),
                                write.offset,
                                write.source.size(),
                                write.source.data()
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
                                barrier.size = copy.source.size();
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
                                copy.source.size(),
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
                if (accepted && &queue == &session.queue())
                {
                    auto& frame = session.frames_[session.next_frame_];
                    if (frame.phase == DeviceSession::EFramePhase::COLLECTING ||
                        frame.phase == DeviceSession::EFramePhase::PREPARING)
                    {
                        frame.accepted_serials[session.queue_roles_[0]] = queue.submitted();
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
            if (!session.deviceLost() && !session.staging_arena_->canAcquire(entry->minimum_range))
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
                                     : RenderResult<void>{}
            );
        }
    }

    UploadContext::Task UploadContext::writeBuffers(
        std::span<const BufferWrite> writes,
        std::span<const BufferCopy> copies,
        std::span<const BufferFill> fills
    ) noexcept
    {
        return write(writes, copies, fills, nullptr, {}, {}, nullptr, false);
    }

    UploadContext::Task UploadContext::updateBuffers(
        std::span<const BufferWrite> writes,
        std::span<const BufferCopy> copies,
        std::span<const BufferFill> fills
    ) noexcept
    {
        return write(writes, copies, fills, nullptr, {}, {}, nullptr, true);
    }

    UploadContext::Task UploadContext::writeImage(
        gapi::vk::VmaImage& target,
        std::span<const std::byte> source,
        std::vector<ImageTransferRegion> regions,
        const gapi::vk::VmaImage* clone
    ) noexcept
    {
        return write({}, {}, {}, &target, source, std::move(regions), clone, false);
    }

    UploadContext::Task UploadContext::write(
        std::span<const BufferWrite> writes,
        std::span<const BufferCopy> gpu_copies,
        std::span<const BufferFill> fills,
        gapi::vk::VmaImage* image,
        std::span<const std::byte> source,
        std::vector<ImageTransferRegion> regions,
        const gapi::vk::VmaImage* clone,
        bool inline_updates
    ) noexcept
    {
        std::size_t pending_bytes = 0;
        if (!image)
        {
            for (const auto& input : writes)
            {
                if (!input.target)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const bool invalid = input.source.empty() || input.source.size() % 4 || input.offset % 4 ||
                                     (inline_updates && input.source.size() > 65536) ||
                                     input.offset > input.target->size() ||
                                     input.source.size() > input.target->size() - input.offset ||
                                     !(input.target->usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                if (invalid)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
                if (input.source.size() > options.pending_bytes - pending_bytes)
                {
                    co_return cxx::unexpected(RenderError{kCapacity});
                }
                pending_bytes += input.source.size();
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
        for (const auto& copy : gpu_copies)
        {
            if (!copy.source || !copy.target)
            {
                co_return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto& range = copy.range;
            const bool invalid =
                !range.size || (range.srcOffset | range.dstOffset | range.size) % 4 ||
                range.srcOffset > copy.source->size() || range.size > copy.source->size() - range.srcOffset ||
                range.dstOffset > copy.target->size() || range.size > copy.target->size() - range.dstOffset ||
                !(copy.source->usage() & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
                !(copy.target->usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            if (invalid)
            {
                co_return cxx::unexpected(RenderError{kInvalidArgument});
            }
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
        if (!image && writes.empty() && gpu_copies.empty() && fills.empty())
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
        std::vector<BatchEntry::Copy> copies;
        if (!inline_updates)
        {
            copies.reserve(options.staging_pages);
        }
        std::size_t index = 0, offset = 0;
        std::uint32_t row = 0;
        const auto count = image ? regions.size() : inline_updates ? 0 : writes.size();
        while (index < count || !accepted_any)
        {
            if (stop.stop_requested())
            {
                co_return failure(RenderError{kWorkStopped});
            }
            copies.clear();
            while (index < count && copies.size() < copies.capacity())
            {
                const auto* region = image ? &regions[index] : nullptr;
                const auto row_bytes =
                    region ? std::size_t((region->extent.width - 1) / region->block_extent + 1) * region->block_bytes
                           : 0;
                const auto remaining =
                    region ? std::size_t((region->extent.height - 1) / region->block_extent + 1 - row) * row_bytes
                           : writes[index].source.size() - offset;
                auto range =
                    session.staging_arena_->acquire(std::min(remaining, options.chunk_bytes), region ? row_bytes : 4);
                if (!range)
                {
                    break;
                }
                const auto bytes = range->bytes().size();
                const auto rows = region ? std::uint32_t(bytes / row_bytes) : 0;
                const bool region_final = bytes == remaining;
                const bool final = region ? region_final && index + 1 == count : true;
                BatchEntry::Copy chunk{std::move(*range)};
                chunk.buffer = region ? nullptr : writes[index].target;
                chunk.image = image;
                chunk.clone = clone;
                chunk.offset = region ? 0 : writes[index].offset + offset;
                chunk.region = region ? *region : ImageTransferRegion{};
                chunk.source = region ? source : writes[index].source.subspan(offset, bytes);
                chunk.row = row;
                chunk.rows = rows;
                chunk.first = !accepted_any && copies.empty();
                chunk.final = final;
                copies.push_back(std::move(chunk));
                offset += bytes;
                row += rows;
                if (region_final)
                {
                    ++index;
                    offset = 0;
                    row = 0;
                }
            }
            if (copies.empty() && index < count)
            {
                BatchEntry wait{this, &copy_queue, {}};
                wait.waiting_range = true;
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
                                    std::memcpy(target.data(), copy.source.data(), target.size());
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
            if (!accepted_any)
            {
                entry.gpu_copies = gpu_copies;
                entry.fills = fills;
                entry.updates = inline_updates ? writes : std::span<const BufferWrite>{};
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
