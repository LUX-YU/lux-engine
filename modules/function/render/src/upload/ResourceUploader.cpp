#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/memory/StagingArena.hpp>
#include <lux/engine/render/upload/ImageTransfer.hpp>
#include <numeric>

namespace lux::render::detail
{
    using UploadClock = std::chrono::steady_clock;

    static std::uint64_t elapsed(UploadClock::time_point start) noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(UploadClock::now() - start).count();
    }

    static inline std::size_t bufferBytes(
        const BufferWrite& input,
        std::uint32_t element_bytes,
        std::uint32_t source_stride
    ) noexcept
    {
        return source_stride ? ((input.source.size() - element_bytes) / source_stride + 1) * element_bytes
                             : input.source.size();
    }

    struct ResourceUploader::Measurement final
    {
        SubmissionCallback completion;
        ResourceUploader* uploads = nullptr;
        std::size_t index = 0;
        std::uint32_t bits = 0;
        std::uint64_t pending_copy_serial = 0;
    };

    struct ResourceUploader::Copy final
    {
        StagingRange staging;
        nri::Buffer* buffer = nullptr;
        nri::Texture* image = nullptr;
        const nri::Texture* clone = nullptr;
        std::uint64_t offset = 0;
        ImageTransferRegion region;
        std::span<const std::byte> source;
        std::span<const BufferWrite> inputs;
        std::size_t input_offset = 0, bytes = 0;
        std::uint32_t row = 0, rows = 0, row_pitch = 0;
        bool first = false, final = false;
    };

    struct ResourceUploader::BatchEntry final
    {
        ResourceUploader* uploads;
        SubmissionQueue* queue;
        std::span<Copy> copies;
        std::span<const BufferZero> fills;
        BatchEntry* next = nullptr;
        void* receiver = nullptr;
        void (*complete)(void*, RenderResult<void>) noexcept = nullptr;
        std::uint32_t frame_slot = UINT32_MAX;
        bool (*stopping)(void*) noexcept = nullptr;
        bool acquiring = false;
        bool waiting_range = false;
        std::size_t minimum_range = 4;
        SubmissionFrontiers* progress = nullptr;
        std::uint64_t copy_serial = 0;
    };

    class ResourceUploader::BatchSender final
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
                entry.uploads->enqueue(entry);
            }
        };

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            return Operation<Receiver>{entry_, std::move(receiver)};
        }

    private:
        BatchEntry& entry_;
    };

    ResourceUploader::ResourceUploader(
        DeviceSession& device,
        TransferOptions settings,
        execution::CpuScheduler cpu_scheduler
    ) noexcept
        : device_(device), options_(settings), cpu_scheduler_(std::move(cpu_scheduler))
    {
    }

    ResourceUploader::~ResourceUploader() noexcept
    {
        for (auto& copy : frame_copies_)
        {
            staging_arena_->cancel(copy.staging);
        }
        if (mapped_timestamps_)
        {
            device_.nriCore().UnmapBuffer(*timestamp_results_);
        }
        device_.nriCore().DestroyBuffer(timestamp_results_);
        for (auto* pool : timestamps_)
        {
            device_.nriCore().DestroyQueryPool(pool);
        }
    }

    RenderResult<std::unique_ptr<ResourceUploader>> ResourceUploader::create(
        DeviceSession& device_,
        const TransferOptions& options_,
        execution::CpuScheduler cpu_scheduler_
    ) noexcept
    {
        const bool invalid = options_.chunk_bytes < 256 || options_.chunk_bytes > 16 * 1024 * 1024 ||
                             options_.staging_pages == 0 || options_.staging_pages > 16 ||
                             options_.pending_bytes < options_.chunk_bytes || options_.batch_entries == 0 ||
                             options_.window_regions == 0;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        auto state = std::unique_ptr<ResourceUploader>(new ResourceUploader(device_, options_, cpu_scheduler_));
        auto& transfer = device_.queue(EQueueRole::TRANSFER);
        const auto& granularity = transfer.properties().image_transfer_granularity;
        const bool row_chunks = granularity[0] == 1 && granularity[1] == 1 && granularity[2] == 1;
        state->copy_queue_ = row_chunks ? &transfer : &device_.queue();
        state->copy_timestamp_bits_ = state->copy_queue_->properties().timestamp_bits;
        state->graphics_timestamp_bits_ = device_.queue().properties().timestamp_bits;
        if (options_.measure_gpu && (state->copy_timestamp_bits_ || state->graphics_timestamp_bits_))
        {
            state->graphics_measurement_base_ =
                state->copy_queue_ == &device_.queue() ? 0 : state->copy_queue_->capacity();
            const auto query_slots = state->graphics_measurement_base_ + device_.queue().capacity();
            state->measurements_ = std::make_unique<ResourceUploader::Measurement[]>(query_slots);
            for (std::size_t i = 0; i < query_slots; ++i)
            {
                state->measurements_[i].uploads = state.get();
                state->measurements_[i].index = i;
            }
            const std::array queues{state->copy_queue_, &device_.queue()};
            for (std::size_t i = 0; i < queues.size(); ++i)
            {
                if (i == 1 && state->graphics_measurement_base_ == 0)
                {
                    continue;
                }
                if (queues[i]->properties().timestamp_bits == 0)
                {
                    continue;
                }
                const auto type = queues[i]->properties().type == nri::QueueType::COPY
                                      ? nri::QueryType::TIMESTAMP_COPY_QUEUE
                                      : nri::QueryType::TIMESTAMP;
                const nri::QueryPoolDesc query{type, queues[i]->capacity() * 2};
                const auto result = device_.nriCore().CreateQueryPool(device_.device(), query, state->timestamps_[i]);
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
            // Distinct in-flight pairs must not share a non-coherent memory atom.
            state->query_result_stride_ = std::lcm<std::uint64_t>(device_.uploadAlignment(), 16);
            const nri::BufferDesc description{query_slots * state->query_result_stride_, 0, nri::BufferUsageBits::NONE};
            const auto result =
                device_.nriCore()
                    .CreatePlacedBuffer(device_.device(), NriHostReadbackHeap, description, state->timestamp_results_);
            if (result != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            state->mapped_timestamps_ =
                static_cast<std::uint64_t*>(device_.nriCore().MapBuffer(*state->timestamp_results_, 0, description.size)
                );
            if (!state->mapped_timestamps_)
            {
                return cxx::unexpected(nativeError(nri::Result::FAILURE));
            }
        }
        auto arena =
            StagingArena::create(device_, options_.chunk_bytes, options_.staging_pages, device_.uploadAlignment());
        if (!arena)
        {
            return cxx::unexpected(arena.error());
        }
        state->staging_arena_ = std::move(*arena);
        state->statistics_.staging_bytes = options_.chunk_bytes * options_.staging_pages;
        state->batch_entries_.reserve(options_.batch_entries);
        state->frame_copies_.reserve(options_.window_regions);
        return state;
    }

    bool ResourceUploader::reserveSourceBytes(std::size_t bytes) noexcept
    {
        if (bytes > options_.pending_bytes - statistics_.pending_bytes)
        {
            return false;
        }
        statistics_.pending_bytes += bytes;
        statistics_.pending_peak = std::max(statistics_.pending_peak, statistics_.pending_bytes);
        return true;
    }

    void ResourceUploader::releaseSourceBytes(std::size_t bytes) noexcept
    {
        statistics_.pending_bytes -= bytes;
    }

    static void bufferBarrier(
        const nri::CoreInterface& nri_core,
        nri::CommandBuffer& command,
        nri::Buffer& buffer,
        nri::AccessStage before,
        nri::AccessStage after
    ) noexcept
    {
        const nri::BufferBarrierDesc barrier{&buffer, before, after};
        const nri::BarrierDesc dependency{nullptr, 0, &barrier, 1};
        nri_core.CmdBarrier(command, dependency);
    }

    static void recordBuffer(
        const nri::CoreInterface& nri_core,
        nri::CommandBuffer& command,
        nri::Buffer& target,
        const StagingRange& range,
        std::size_t offset,
        std::size_t bytes,
        bool acquiring,
        bool final
    ) noexcept
    {
        const auto access = bufferAccess(nri_core.GetBufferDesc(target).usage);
        if (!acquiring)
        {
            bufferBarrier(
                nri_core,
                command,
                target,
                {access, nri::StageBits::ALL},
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}
            );
            nri_core.CmdCopyBuffer(command, target, offset, *range.buffer, range.offset, bytes);
        }
        if (final)
        {
            bufferBarrier(
                nri_core,
                command,
                target,
                {acquiring ? nri::AccessBits::NONE : nri::AccessBits::COPY_DESTINATION,
                 acquiring ? nri::StageBits::NONE : nri::StageBits::COPY},
                {access, nri::StageBits::ALL}
            );
        }
    }

    static void recordImage(
        const nri::CoreInterface& nri_core,
        nri::CommandBuffer& command,
        nri::Texture& target,
        const StagingRange& range,
        const ImageTransferRegion& input,
        const nri::Texture* clone,
        std::uint32_t row,
        std::uint32_t rows,
        std::uint32_t row_pitch,
        std::uint32_t slice_alignment,
        bool first,
        SubmissionQueue& copy,
        SubmissionQueue& graphics,
        bool acquiring,
        bool final
    ) noexcept
    {
        const bool distinct = &copy != &graphics;
        const auto& description = nri_core.GetTextureDesc(target);
        const bool cross_family = description.sharingMode == nri::SharingMode::EXCLUSIVE &&
                                  copy.properties().family != graphics.properties().family;
        nri::TextureBarrierDesc barrier{};
        barrier.texture = &target;
        barrier.mipNum = description.mipNum;
        barrier.layerNum = description.layerNum;
        barrier.planes = nri::PlaneBits::COLOR;
        auto emit = [&](const nri::TextureBarrierDesc& value) noexcept
        {
            const nri::BarrierDesc dependency{nullptr, 0, nullptr, 0, &value, 1};
            nri_core.CmdBarrier(command, dependency);
        };
        if (!acquiring)
        {
            if (first)
            {
                barrier.before = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::NONE};
                barrier
                    .after = {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
                emit(barrier);
                if (clone)
                {
                    auto original = barrier;
                    original.texture = const_cast<nri::Texture*>(clone);
                    original
                        .before = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::ALL};
                    original.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
                    emit(original);
                    for (nri::Dim_t mip = 0; mip < description.mipNum; ++mip)
                    {
                        for (nri::Dim_t layer = 0; layer < description.layerNum; ++layer)
                        {
                            nri::TextureRegionDesc region{};
                            region.width = std::max(1u, std::uint32_t(description.width) >> mip);
                            region.height = std::max(1u, std::uint32_t(description.height) >> mip);
                            region.depth = 1;
                            region.mipOffset = mip;
                            region.layerOffset = layer;
                            region.planes = nri::PlaneBits::COLOR;
                            nri_core.CmdCopyTexture(command, target, &region, *clone, &region);
                        }
                    }
                    std::swap(original.before, original.after);
                    emit(original);
                    barrier.before = barrier.after;
                    emit(barrier); // Clone writes precede region patch writes.
                }
            }
            auto region = input.texture;
            region.y += row * input.block_extent;
            region.height =
                std::min(rows * input.block_extent, std::uint32_t(input.texture.height - row * input.block_extent));
            const auto slice_unit = std::lcm(row_pitch, slice_alignment);
            const auto slice_pitch = ((row_pitch * rows + slice_unit - 1) / slice_unit) * slice_unit;
            const nri::TextureDataLayoutDesc layout{range.offset, row_pitch, slice_pitch};
            nri_core.CmdUploadBufferToTexture(command, target, region, *range.buffer, layout);
        }
        if (final)
        {
            barrier.before = {
                acquiring ? nri::AccessBits::NONE : nri::AccessBits::COPY_DESTINATION,
                acquiring && !cross_family ? nri::Layout::SHADER_RESOURCE : nri::Layout::COPY_DESTINATION,
                acquiring ? nri::StageBits::NONE : nri::StageBits::COPY
            };
            barrier.after = {
                !distinct || acquiring ? nri::AccessBits::SHADER_RESOURCE : nri::AccessBits::NONE,
                nri::Layout::SHADER_RESOURCE,
                !distinct || acquiring ? nri::StageBits::ALL : nri::StageBits::NONE
            };
            if (cross_family)
            {
                barrier.srcQueue = &copy.native();
                barrier.dstQueue = &graphics.native();
            }
            emit(barrier);
        }
    }

    RenderResult<void> ResourceUploader::collectUploadTimestamps(std::size_t index, std::uint32_t bits) noexcept
    {
        const auto result = device_.invalidateMappedRange(*timestamp_results_, index * query_result_stride_, 16);
        if (result != nri::Result::SUCCESS)
        {
            ++statistics_.gpu_query_errors;
            return cxx::unexpected(nativeError(result));
        }
        const auto* values = mapped_timestamps_ + index * (query_result_stride_ / sizeof(std::uint64_t));
        const auto mask = bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
        statistics_.gpu_ns += std::uint64_t(
            double((values[1] - values[0]) & mask) * 1'000'000'000.0 / device_.description().other.timestampFrequencyHz
        );
        ++statistics_.gpu_samples;
        return {};
    }

    RenderResult<std::size_t> ResourceUploader::stageBuffer(const BufferWrite& write, std::size_t unit) noexcept
    {
        if (frame_copies_.size() == options_.window_regions)
        {
            return 0;
        }
        auto range = staging_arena_->acquire(std::min(write.source.size(), options_.chunk_bytes), unit);
        if (!range)
        {
            return 0;
        }
        const auto bytes = range->bytes.size();
        std::memcpy(range->bytes.data(), write.source.data(), bytes);
        if (auto result = staging_arena_->flush(*range); !result)
        {
            staging_arena_->cancel(*range);
            return cxx::unexpected(result.error());
        }
        Copy copy{std::move(*range)};
        copy.buffer = write.target;
        copy.offset = write.offset;
        copy.bytes = bytes;
        copy.final = true;
        frame_copies_.push_back(std::move(copy));
        statistics_.copied_bytes += bytes;
        return bytes;
    }

    bool ResourceUploader::hasFrameCopies() const noexcept
    {
        return !frame_copies_.empty();
    }

    void ResourceUploader::recordFrameCopies(nri::CommandBuffer& command) noexcept
    {
        const auto& nri_core = device_.nriCore();
        std::size_t first = 0;
        while (first < frame_copies_.size())
        {
            auto* target = frame_copies_[first].buffer;
            auto end = frame_copies_[first].offset + frame_copies_[first].bytes;
            auto last = first + 1;
            while (last < frame_copies_.size())
            {
                const auto& next = frame_copies_[last];
                if (next.buffer != target || next.offset < end)
                {
                    break;
                }
                end = next.offset + next.bytes;
                ++last;
            }
            // NRI barriers cover a whole Buffer. Disjoint, ordered writes share one transition;
            // overlapping or reverse ranges start another segment and retain their write dependency.
            const auto access = bufferAccess(nri_core.GetBufferDesc(*target).usage);
            bufferBarrier(
                nri_core,
                command,
                *target,
                {access, nri::StageBits::ALL},
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}
            );
            for (auto i = first; i < last; ++i)
            {
                const auto& copy = frame_copies_[i];
                nri_core.CmdCopyBuffer(
                    command,
                    *target,
                    copy.offset,
                    *copy.staging.buffer,
                    copy.staging.offset,
                    copy.bytes
                );
            }
            bufferBarrier(
                nri_core,
                command,
                *target,
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                {access, nri::StageBits::ALL}
            );
            first = last;
        }
    }

    void ResourceUploader::finishFrameCopySubmission(RenderResult<void> result) noexcept
    {
        for (auto& copy : frame_copies_)
        {
            if (result)
            {
                staging_arena_->markSubmitted(copy.staging, device_.queue());
                ++statistics_.copy_regions;
            }
            else
            {
                staging_arena_->cancel(copy.staging);
            }
        }
        frame_copies_.clear();
    }

    bool ResourceUploader::canStage(std::uint32_t frame_slot) const noexcept
    {
        if (frame_slot != UINT32_MAX)
        {
            return true;
        }
        return background_allowed_;
    }

    void ResourceUploader::enqueue(BatchEntry& entry) noexcept
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

    void ResourceUploader::submitReadyTransfers() noexcept
    {
        // Complete only this bounded prefix. Receiver reentry may append work for the next flush.
        auto* boundary = ready_tail_;
        BatchEntry* blocked_head = nullptr;
        BatchEntry* blocked_tail = nullptr;
        while (ready_head_ && boundary)
        {
            auto& first = *ready_head_;
            const bool acquiring = first.acquiring;
            auto& source_queue = *first.queue;
            auto& queue = acquiring ? device_.queue() : source_queue;
            batch_entries_.clear();
            while (ready_head_ && batch_entries_.size() < batch_entries_.capacity())
            {
                auto* entry = ready_head_;
                const bool same_submission = entry->queue == &source_queue && entry->acquiring == acquiring;
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
            nri::FenceSubmitDesc wait{};
            if (acquiring)
            {
                std::uint64_t producer = 0;
                for (const auto* entry : batch_entries_)
                {
                    producer = std::max(producer, entry->copy_serial);
                }
                auto dependency = source_queue.dependency(producer, nri::StageBits::ALL);
                if (dependency)
                {
                    wait = *dependency;
                }
                else
                {
                    accepted = cxx::unexpected(dependency.error());
                }
            }
            const auto bits = &queue == &device_.queue() ? graphics_timestamp_bits_ : copy_timestamp_bits_;
            const auto pool_index = &queue == &device_.queue() && graphics_measurement_base_ ? 1 : 0;
            auto* query_pool = timestamps_[pool_index];
            const bool timestamp_requested = query_pool && bits && !acquiring;
            bool timestamp = false;
            // NRI/Vulkan permits timestamp writes on a pure Copy queue, but query result Copy
            // requires Graphics/Compute. Resolve in existing Graphics work only: its normal
            // upload dependency or an already observed Copy frontier proves availability.
            auto resolve_through = copy_queue_->completed();
            if (acquiring && &source_queue == copy_queue_)
            {
                resolve_through = std::max(resolve_through, wait.value);
            }
            std::size_t query_index = 0;
            const auto started = options_.measure_cpu ? UploadClock::now() : UploadClock::time_point{};
            if (accepted)
            {
                accepted = queue.recordAndSubmit(
                    [&](nri::CommandBuffer& command, std::uint32_t slot) noexcept -> RenderResult<void>
                    {
                        const auto& nri_core = device_.nriCore();
                        query_index = (pool_index ? graphics_measurement_base_ : 0) + slot;
                        if (timestamp_requested)
                        {
                            const auto& measurement = measurements_[query_index];
                            timestamp = !measurement.pending_copy_serial && !measurement.completion.armed();
                        }
                        if (timestamp)
                        {
                            // The pair's preceding GPU use and CPU read have both ended.
                            nri_core.ResetQueries(*query_pool, slot * 2, 2);
                            nri_core.CmdEndQuery(command, *query_pool, slot * 2);
                        }
                        for (auto* entry : batch_entries_)
                        {
                            for (const auto& fill : entry->fills)
                            {
                                const auto access = bufferAccess(nri_core.GetBufferDesc(*fill.target).usage);
                                bufferBarrier(
                                    nri_core,
                                    command,
                                    *fill.target,
                                    {access, nri::StageBits::ALL},
                                    {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}
                                );
                                nri_core.CmdZeroBuffer(command, *fill.target, fill.offset, fill.size);
                                bufferBarrier(
                                    nri_core,
                                    command,
                                    *fill.target,
                                    {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                                    {access, nri::StageBits::ALL}
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
                                    recordBuffer(
                                        nri_core,
                                        command,
                                        *copy.buffer,
                                        copy.staging,
                                        copy.offset,
                                        copy.bytes,
                                        acquiring,
                                        copy.final
                                    );
                                }
                                else
                                {
                                    recordImage(
                                        nri_core,
                                        command,
                                        *copy.image,
                                        copy.staging,
                                        copy.region,
                                        copy.clone,
                                        copy.row,
                                        copy.rows,
                                        copy.row_pitch,
                                        device_.description().memoryAlignment.uploadBufferTextureSlice,
                                        copy.first,
                                        source_queue,
                                        device_.queue(),
                                        acquiring,
                                        copy.final
                                    );
                                }
                            }
                        }
                        if (timestamp)
                        {
                            nri_core.CmdEndQuery(command, *query_pool, slot * 2 + 1);
                            if (&queue == &device_.queue())
                            {
                                nri_core.CmdCopyQueries(
                                    command,
                                    *query_pool,
                                    slot * 2,
                                    2,
                                    *timestamp_results_,
                                    query_index * query_result_stride_
                                );
                            }
                        }
                        if (&queue == &device_.queue())
                        {
                            for (std::uint32_t i = 0; i < graphics_measurement_base_; ++i)
                            {
                                const auto serial = measurements_[i].pending_copy_serial;
                                if (serial && serial <= resolve_through)
                                {
                                    nri_core.CmdCopyQueries(
                                        command,
                                        *timestamps_[0],
                                        i * 2,
                                        2,
                                        *timestamp_results_,
                                        i * query_result_stride_
                                    );
                                }
                            }
                        }
                        return {};
                    },
                    acquiring ? std::span<const nri::FenceSubmitDesc>(&wait, 1)
                              : std::span<const nri::FenceSubmitDesc>{}
                );
            }
            if (options_.measure_cpu)
            {
                statistics_.record_submit_ns += elapsed(started);
            }
            const auto accepted_serial = accepted ? queue.submitted() : 0;
            if (!accepted && (accepted.error().type == kDeviceCapacity || accepted.error().type == kDeviceBusy))
            {
                // Put blocked entries ahead of work appended by later completions. They keep their
                // prepared bytes and original receivers; a failed admission has no GPU side effect.
                for (auto* entry : batch_entries_)
                {
                    if (blocked_tail)
                    {
                        blocked_tail->next = entry;
                    }
                    else
                    {
                        blocked_head = entry;
                    }
                    blocked_tail = entry;
                }
                continue;
            }
            if (accepted)
            {
                acquiring ? ++statistics_.acquire_submissions : ++statistics_.copy_submissions;
                for (auto* entry : batch_entries_)
                {
                    if (entry->progress)
                    {
                        const auto index = device_.queueIndex(queue);
                        auto& serial = entry->progress->serials[index];
                        serial = std::max(serial, queue.submitted());
                    }
                }
                const auto observe_measurement = [&](Measurement& measurement) noexcept
                {
                    queue.arm(
                        queue.submitted(),
                        measurement.completion,
                        [](void* value, RenderResult<void> completed) noexcept
                        {
                            auto& state = *static_cast<Measurement*>(value);
                            if (completed)
                            {
                                (void)state.uploads->collectUploadTimestamps(state.index, state.bits);
                            }
                        },
                        &measurement
                    );
                };
                if (&queue == &device_.queue())
                {
                    for (std::uint32_t i = 0; i < graphics_measurement_base_; ++i)
                    {
                        auto& measurement = measurements_[i];
                        if (measurement.pending_copy_serial && measurement.pending_copy_serial <= resolve_through)
                        {
                            measurement.pending_copy_serial = 0;
                            observe_measurement(measurement);
                        }
                    }
                }
                if (timestamp)
                {
                    auto& measurement = measurements_[query_index];
                    measurement.bits = bits;
                    if (&queue == &device_.queue())
                    {
                        observe_measurement(measurement);
                    }
                    else
                    {
                        measurement.pending_copy_serial = queue.submitted();
                    }
                }
                else if (timestamp_requested)
                {
                    ++statistics_.gpu_samples_skipped;
                }
                if (!acquiring)
                {
                    for (auto* entry : batch_entries_)
                    {
                        for (auto& copy : entry->copies)
                        {
                            staging_arena_->markSubmitted(copy.staging, queue);
                            ++statistics_.copy_regions;
                        }
                    }
                }
            }
            for (auto* entry : batch_entries_)
            {
                const bool needs_acquire =
                    accepted && !acquiring && entry->queue != &device_.queue() &&
                    std::ranges::any_of(entry->copies, [](const auto& copy) { return copy.final; });
                if (needs_acquire)
                {
                    entry->copy_serial = accepted_serial;
                    entry->acquiring = true;
                    enqueue(*entry);
                }
                else
                {
                    const auto complete = entry->complete;
                    auto* receiver = entry->receiver;
                    complete(receiver, accepted);
                    // Neither the operation nor its borrowed entry is read after completion.
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
        auto* range_boundary = range_waiters_tail_;
        while (range_waiters_ && range_boundary)
        {
            auto* entry = range_waiters_;
            const bool stopping = entry->stopping(entry->receiver);
            if (!stopping && !device_.deviceLost() &&
                (!canStage(entry->frame_slot) || !staging_arena_->canAcquire(
                                                     entry->minimum_range,
                                                     entry->frame_slot == UINT32_MAX && reserve_frame_staging_
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
                device_.deviceLost() ? RenderResult<void>{cxx::unexpected(nativeError(nri::Result::DEVICE_LOST))}
                : stopping           ? RenderResult<void>{cxx::unexpected(RenderError{kUploadStopped})}
                                     : RenderResult<void>{}
            );
        }
    }

    ResourceUploader::Task ResourceUploader::writeBuffers(
        std::span<const BufferWrite> writes,
        std::span<const BufferZero> fills,
        std::uint32_t frame_slot,
        SubmissionFrontiers* progress
    ) noexcept
    {
        return write(writes, fills, nullptr, {}, {}, nullptr, frame_slot, 0, 0, progress);
    }

    ResourceUploader::Task ResourceUploader::writeImage(
        nri::Texture& target,
        std::span<const std::byte> source,
        std::vector<ImageTransferRegion> regions,
        const nri::Texture* clone
    ) noexcept
    {
        return write({}, {}, &target, source, std::move(regions), clone, UINT32_MAX);
    }

    ResourceUploader::Task ResourceUploader::writeBuffers(
        std::span<const BufferWrite> writes,
        std::uint32_t element_bytes,
        std::uint32_t source_stride,
        std::uint32_t frame_slot,
        SubmissionFrontiers* progress
    ) noexcept
    {
        return write(writes, {}, nullptr, {}, {}, nullptr, frame_slot, element_bytes, source_stride, progress);
    }

    ResourceUploader::Task ResourceUploader::write(
        std::span<const BufferWrite> writes,
        std::span<const BufferZero> fills,
        nri::Texture* image,
        std::span<const std::byte> source,
        std::vector<ImageTransferRegion> regions,
        const nri::Texture* clone,
        std::uint32_t frame_slot,
        std::uint32_t element_bytes,
        std::uint32_t source_stride,
        SubmissionFrontiers* progress
    ) noexcept
    {
        const bool strided = element_bytes != 0 || source_stride != 0;
        const bool invalid_layout = strided && (!element_bytes || element_bytes % 4 || source_stride < element_bytes);
        if (invalid_layout)
        {
            co_return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::size_t pending_bytes = 0;
        std::size_t buffer_runs = 0;
        const nri::Buffer* previous_target = nullptr;
        std::uint64_t previous_end = 0;
        if (!image)
        {
            for (const auto& input : writes)
            {
                if (!input.target)
                {
                    co_return cxx::unexpected(RenderError{kDeviceArgument});
                }
                const bool invalid_source = strided ? input.source.size() < element_bytes ||
                                                          (input.source.size() - element_bytes) % source_stride != 0
                                                    : input.source.empty();
                if (invalid_source)
                {
                    co_return cxx::unexpected(RenderError{kDeviceArgument});
                }
                const auto bytes = bufferBytes(input, element_bytes, source_stride);
                const bool invalid = bytes % 4 || input.offset % 4 ||
                                     input.offset > device_.nriCore().GetBufferDesc(*input.target).size ||
                                     bytes > device_.nriCore().GetBufferDesc(*input.target).size - input.offset;
                if (invalid)
                {
                    co_return cxx::unexpected(RenderError{kDeviceArgument});
                }
                if (bytes > options_.pending_bytes - pending_bytes)
                {
                    co_return cxx::unexpected(RenderError{kDeviceCapacity});
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
                co_return cxx::unexpected(RenderError{kDeviceArgument});
            }
            const auto& description = device_.nriCore().GetTextureDesc(*image);
            const bool invalid = description.sampleNum != 1;
            if (invalid)
            {
                co_return cxx::unexpected(RenderError{kDeviceArgument});
            }
            for (const auto& region : regions)
            {
                if (auto valid = imageRegionBytes(description, region, source.size(), options_.chunk_bytes); !valid)
                {
                    co_return cxx::unexpected(valid.error());
                }
                const auto bytes =
                    std::size_t((region.texture.width - 1) / region.block_extent + 1) * region.block_bytes;
                const auto alignment = device_.description().memoryAlignment.uploadBufferTextureRow;
                if ((bytes + alignment - 1) / alignment * alignment > options_.chunk_bytes)
                {
                    co_return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
            pending_bytes = source.size();
        }
        for (const auto& fill : fills)
        {
            if (!fill.target)
            {
                co_return cxx::unexpected(RenderError{kDeviceArgument});
            }
            const bool invalid = !fill.size || (fill.offset | fill.size) % 4 ||
                                 fill.offset > device_.nriCore().GetBufferDesc(*fill.target).size ||
                                 fill.size > device_.nriCore().GetBufferDesc(*fill.target).size - fill.offset;
            if (invalid)
            {
                co_return cxx::unexpected(RenderError{kDeviceArgument});
            }
        }
        if (!image && writes.empty() && fills.empty())
        {
            co_return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (!reserveSourceBytes(pending_bytes))
        {
            co_return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        cxx::scope_exit budget([&]() noexcept { releaseSourceBytes(pending_bytes); });
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        auto& copy_queue_ = !image || clone ? device_.queue() : *this->copy_queue_;
        bool accepted_any = false;
        const auto failure = [&](RenderError error) noexcept
        {
            const bool lost =
                error.type == kDeviceFailure && error.args[0] == static_cast<std::uint32_t>(nri::Result::DEVICE_LOST);
            return cxx::unexpected(accepted_any && lost ? RenderError{kOutcomeUnknown} : error);
        };
        std::vector<Copy> copies;
        cxx::scope_exit release_ranges(
            [&]() noexcept
            {
                for (auto& copy : copies)
                {
                    staging_arena_->cancel(copy.staging);
                }
            }
        );
        const auto ranges = image ? regions.size() : buffer_runs;
        copies.reserve(std::min(options_.window_regions, std::max(options_.staging_pages, ranges)));
        std::size_t index = 0, offset = 0, run_end = 0, run_remaining = 0;
        std::uint32_t row = 0;
        const auto count = image ? regions.size() : writes.size();
        while (index < count || !accepted_any)
        {
            if (stop.stop_requested())
            {
                co_return failure(RenderError{kUploadStopped});
            }
            copies.clear();
            while (index < count && copies.size() < copies.capacity() && canStage(frame_slot))
            {
                const auto* region = image ? &regions[index] : nullptr;
                const auto row_bytes =
                    region ? std::size_t((region->texture.width - 1) / region->block_extent + 1) * region->block_bytes
                           : 0;
                const auto row_alignment = device_.description().memoryAlignment.uploadBufferTextureRow;
                const auto row_pitch = region ? (row_bytes + row_alignment - 1) / row_alignment * row_alignment : 0;
                if (!region && run_remaining == 0)
                {
                    run_end = index + 1;
                    run_remaining = bufferBytes(writes[index], element_bytes, source_stride) - offset;
                    auto end_offset = writes[index].offset + bufferBytes(writes[index], element_bytes, source_stride);
                    while (run_end < count && writes[run_end].target == writes[index].target &&
                           writes[run_end].offset == end_offset)
                    {
                        const auto bytes = bufferBytes(writes[run_end], element_bytes, source_stride);
                        run_remaining += bytes;
                        end_offset += bytes;
                        ++run_end;
                    }
                }
                const auto remaining =
                    region ? std::size_t((region->texture.height - 1) / region->block_extent + 1 - row) * row_pitch
                           : run_remaining;
                auto range = staging_arena_->acquire(
                    std::min(remaining, options_.chunk_bytes),
                    region ? row_pitch : 4,
                    frame_slot == UINT32_MAX && reserve_frame_staging_
                );
                if (!range)
                {
                    break;
                }
                const auto bytes = range->bytes.size();
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
                    chunk.rows = static_cast<std::uint32_t>(bytes / row_pitch);
                    chunk.row_pitch = static_cast<std::uint32_t>(row_pitch);
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
                        const auto input_bytes = bufferBytes(writes[index], element_bytes, source_stride);
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
                BatchEntry wait{this, &copy_queue_, {}};
                wait.waiting_range = true;
                wait.frame_slot = frame_slot;
                if (image)
                {
                    const auto& region = regions[index];
                    const auto bytes =
                        std::size_t((region.texture.width - 1) / region.block_extent + 1) * region.block_bytes;
                    const auto alignment = device_.description().memoryAlignment.uploadBufferTextureRow;
                    wait.minimum_range = (bytes + alignment - 1) / alignment * alignment;
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
                ++statistics_.prepare_windows;
                const auto prepare_window =
                    [&copies, element_bytes, source_stride, measure = options_.measure_cpu]() noexcept
                {
                    const auto started = measure ? UploadClock::now() : UploadClock::time_point{};
                    for (auto& copy : copies)
                    {
                        auto target = copy.staging.bytes;
                        if (copy.buffer)
                        {
                            auto offset = copy.input_offset;
                            std::size_t packed = 0;
                            for (const auto& input : copy.inputs)
                            {
                                const auto count = std::min(
                                    target.size() - packed,
                                    bufferBytes(input, element_bytes, source_stride) - offset
                                );
                                if (!source_stride || source_stride == element_bytes)
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
                            const auto row_bytes =
                                std::size_t((copy.region.texture.width - 1) / copy.region.block_extent + 1) *
                                copy.region.block_bytes;
                            const auto* input = copy.source.data() + copy.region.source_offset +
                                                copy.row * copy.region.source_row_pitch;
                            for (std::uint32_t r = 0; r < copy.rows; ++r)
                            {
                                std::memcpy(
                                    target.data() + r * copy.row_pitch,
                                    input + r * copy.region.source_row_pitch,
                                    row_bytes
                                );
                            }
                        }
                    }
                    return RenderResult<std::uint64_t>{measure ? elapsed(started) : 0};
                };
                std::size_t window_bytes = 0;
                for (const auto& copy : copies)
                {
                    window_bytes += copy.bytes;
                }
                const bool contiguous = !image && (!source_stride || source_stride == element_bytes);
                const bool small_copy = contiguous && window_bytes <= options_.inline_copy_bytes;
                RenderResult<std::uint64_t> prepared;
                if (small_copy)
                {
                    ++statistics_.inline_windows;
                    prepared = prepare_window();
                }
                else
                {
                    prepared = co_await (
                        stdexec::schedule(cpu_scheduler_) | stdexec::then(prepare_window) |
                        stdexec::upon_stopped(
                            []() noexcept
                            { return RenderResult<std::uint64_t>{cxx::unexpected(RenderError{kCpuStopped})}; }
                        )
                    );
                }
                if (!prepared)
                {
                    co_return failure(prepared.error());
                }
                statistics_.prepare_ns += *prepared;
                for (auto& copy : copies)
                {
                    statistics_.copied_bytes +=
                        copy.buffer
                            ? copy.bytes
                            : copy.rows * std::size_t((copy.region.texture.width - 1) / copy.region.block_extent + 1) *
                                  copy.region.block_bytes;
                    if (auto flushed = staging_arena_->flush(copy.staging); !flushed)
                    {
                        co_return failure(flushed.error());
                    }
                }
            }
            if (stop.stop_requested())
            {
                co_return failure(RenderError{kUploadStopped});
            }
            BatchEntry entry{this, &copy_queue_, copies};
            entry.frame_slot = frame_slot;
            entry.progress = progress;
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

    void ResourceUploader::recordCopies(nri::CommandBuffer& command, std::span<const BufferCopy> copies) noexcept
    {
        const auto& core = device_.nriCore();
        for (const auto& copy : copies)
        {
            const std::array<nri::BufferBarrierDesc, 2> barriers{
                {{copy.source,
                  {bufferAccess(core.GetBufferDesc(*copy.source).usage), nri::StageBits::ALL},
                  {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY}},
                 {copy.destination,
                  {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                  {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}}}
            };
            core.CmdBarrier(command, {nullptr, 0, barriers.data(), static_cast<std::uint32_t>(barriers.size())});
            core.CmdCopyBuffer(
                command,
                *copy.destination,
                copy.destination_offset,
                *copy.source,
                copy.source_offset,
                copy.bytes
            );
        }
    }

} // namespace lux::render::detail
