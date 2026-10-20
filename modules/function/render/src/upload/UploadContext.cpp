#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/memory/StagingArena.hpp>
#include <lux/engine/render/upload/ImageTransfer.hpp>
#include <lux/engine/render/upload/UploadContext.hpp>
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

    struct UploadContext::Measurement final
    {
        SubmissionCallback completion;
        UploadContext* uploads = nullptr;
        std::size_t index = 0;
        std::uint32_t bits = 0;
        std::uint64_t pending_copy_serial = 0;
    };

    struct UploadContext::Copy final
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

    struct UploadContext::BatchEntry final
    {
        UploadContext* uploads;
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
        for (auto& copy : frame_copies_)
        {
            session.staging_arena_->cancel(copy.staging);
        }
        if (mapped_timestamps_)
        {
            session.api().UnmapBuffer(*timestamp_results_);
        }
        session.api().DestroyBuffer(timestamp_results_);
        for (auto* pool : timestamps_)
        {
            session.api().DestroyQueryPool(pool);
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
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        auto state = std::unique_ptr<UploadContext>(new UploadContext(*this, options, cpu));
        auto& transfer = queue(EQueueRole::TRANSFER);
        const auto& granularity = transfer.properties().image_transfer_granularity;
        const bool row_chunks = granularity[0] == 1 && granularity[1] == 1 && granularity[2] == 1;
        state->copy_queue = row_chunks ? &transfer : &queue();
        state->copy_timestamp_bits = state->copy_queue->properties().timestamp_bits;
        state->graphics_timestamp_bits = queue().properties().timestamp_bits;
        if (options.measure_gpu && (state->copy_timestamp_bits || state->graphics_timestamp_bits))
        {
            state->graphics_measurement_base_ = state->copy_queue == &queue() ? 0 : state->copy_queue->capacity();
            const auto query_slots = state->graphics_measurement_base_ + queue().capacity();
            state->measurements_ = std::make_unique<UploadContext::Measurement[]>(query_slots);
            for (std::size_t i = 0; i < query_slots; ++i)
            {
                state->measurements_[i].uploads = state.get();
                state->measurements_[i].index = i;
            }
            const std::array queues{state->copy_queue, &queue()};
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
                const auto result = api().CreateQueryPool(device(), query, state->timestamps_[i]);
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
            // Distinct in-flight pairs must not share a non-coherent memory atom.
            state->query_result_stride_ = std::lcm<std::uint64_t>(upload_alignment_, 16);
            const nri::BufferDesc description{query_slots * state->query_result_stride_, 0, nri::BufferUsageBits::NONE};
            const auto result =
                api().CreatePlacedBuffer(device(), NriHostReadbackHeap, description, state->timestamp_results_);
            if (result != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            state->mapped_timestamps_ =
                static_cast<std::uint64_t*>(api().MapBuffer(*state->timestamp_results_, 0, description.size));
            if (!state->mapped_timestamps_)
            {
                return cxx::unexpected(nativeError(nri::Result::FAILURE));
            }
        }
        auto arena = StagingArena::create(*this, options.chunk_bytes, options.staging_pages, upload_alignment_);
        if (!arena)
        {
            return cxx::unexpected(arena.error());
        }
        staging_arena_ = std::move(*arena);
        state->stats.staging_bytes = options.chunk_bytes * options.staging_pages;
        state->batch_entries_.reserve(options.batch_entries);
        state->frame_copies_.reserve(options.window_regions);
        frame_receivers_.reserve(options.window_regions);
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

    static void bufferBarrier(
        const nri::CoreInterface& api,
        nri::CommandBuffer& command,
        nri::Buffer& buffer,
        nri::AccessStage before,
        nri::AccessStage after
    ) noexcept
    {
        const nri::BufferBarrierDesc barrier{&buffer, before, after};
        const nri::BarrierDesc dependency{nullptr, 0, &barrier, 1};
        api.CmdBarrier(command, dependency);
    }

    static void recordBuffer(
        const nri::CoreInterface& api,
        nri::CommandBuffer& command,
        nri::Buffer& target,
        const StagingRange& range,
        std::size_t offset,
        std::size_t bytes,
        bool acquiring,
        bool final
    ) noexcept
    {
        const auto access = bufferAccess(api.GetBufferDesc(target).usage);
        if (!acquiring)
        {
            bufferBarrier(
                api,
                command,
                target,
                {access, nri::StageBits::ALL},
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}
            );
            api.CmdCopyBuffer(command, target, offset, *range.buffer, range.offset, bytes);
        }
        if (final)
        {
            bufferBarrier(
                api,
                command,
                target,
                {acquiring ? nri::AccessBits::NONE : nri::AccessBits::COPY_DESTINATION,
                 acquiring ? nri::StageBits::NONE : nri::StageBits::COPY},
                {access, nri::StageBits::ALL}
            );
        }
    }

    static void recordImage(
        const nri::CoreInterface& api,
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
        const auto& description = api.GetTextureDesc(target);
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
            api.CmdBarrier(command, dependency);
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
                            api.CmdCopyTexture(command, target, &region, *clone, &region);
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
            api.CmdUploadBufferToTexture(command, target, region, *range.buffer, layout);
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

    RenderResult<void> UploadContext::measure(std::size_t index, std::uint32_t bits) noexcept
    {
        const auto result = session.vk().InvalidateBufferRangeVK(*timestamp_results_, index * query_result_stride_, 16);
        if (result != nri::Result::SUCCESS)
        {
            ++stats.gpu_query_errors;
            return cxx::unexpected(nativeError(result));
        }
        const auto* values = mapped_timestamps_ + index * (query_result_stride_ / sizeof(std::uint64_t));
        const auto mask = bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
        stats.gpu_ns += std::uint64_t(
            double((values[1] - values[0]) & mask) * 1'000'000'000.0 / session.description().other.timestampFrequencyHz
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
        const auto bytes = range->bytes.size();
        std::memcpy(range->bytes.data(), write.source.data(), bytes);
        if (auto result = session.staging_arena_->flush(*range); !result)
        {
            session.staging_arena_->cancel(*range);
            return cxx::unexpected(result.error());
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

    bool UploadContext::hasFrameCopies() const noexcept
    {
        return !frame_copies_.empty();
    }

    void UploadContext::recordFrame(nri::CommandBuffer& command) noexcept
    {
        for (const auto& copy : frame_copies_)
        {
            recordBuffer(session.api(), command, *copy.buffer, copy.staging, copy.offset, copy.bytes, false, true);
        }
    }

    void UploadContext::finishFrameCopies(RenderResult<void> result) noexcept
    {
        for (auto& copy : frame_copies_)
        {
            if (result)
            {
                session.staging_arena_->markSubmitted(copy.staging, session.queue());
                ++stats.copy_regions;
            }
            else
            {
                session.staging_arena_->cancel(copy.staging);
            }
        }
        frame_copies_.clear();
    }

    bool UploadContext::canStage(std::uint32_t frame_slot) const noexcept
    {
        if (frame_slot != UINT32_MAX)
        {
            return true;
        }
        return session.backgroundUploadsAllowed();
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
        // Complete only this bounded prefix. Receiver reentry may append work for the next flush.
        auto* boundary = ready_tail_;
        BatchEntry* blocked_head = nullptr;
        BatchEntry* blocked_tail = nullptr;
        while (ready_head_ && boundary)
        {
            auto& first = *ready_head_;
            const bool acquiring = first.acquiring;
            auto& source_queue = *first.queue;
            auto& queue = acquiring ? session.queue() : source_queue;
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
                auto dependency = source_queue.dependency(nri::StageBits::ALL);
                if (dependency)
                {
                    wait = *dependency;
                }
                else
                {
                    accepted = cxx::unexpected(dependency.error());
                }
            }
            const auto bits = &queue == &session.queue() ? graphics_timestamp_bits : copy_timestamp_bits;
            const auto pool_index = &queue == &session.queue() && graphics_measurement_base_ ? 1 : 0;
            auto* query_pool = timestamps_[pool_index];
            const bool timestamp_requested = query_pool && bits && !acquiring;
            bool timestamp = false;
            // NRI/Vulkan permits timestamp writes on a pure Copy queue, but query result Copy
            // requires Graphics/Compute. Resolve in existing Graphics work only: its normal
            // upload dependency or an already observed Copy frontier proves availability.
            auto resolve_through = copy_queue->completed();
            if (acquiring && &source_queue == copy_queue)
            {
                resolve_through = std::max(resolve_through, wait.value);
            }
            std::size_t query_index = 0;
            const auto started = UploadClock::now();
            if (accepted)
            {
                accepted = queue.recordAndSubmit(
                    [&](nri::CommandBuffer& command, std::uint32_t slot) noexcept -> RenderResult<void>
                    {
                        const auto& api = session.api();
                        query_index = (pool_index ? graphics_measurement_base_ : 0) + slot;
                        if (timestamp_requested)
                        {
                            const auto& measurement = measurements_[query_index];
                            timestamp = !measurement.pending_copy_serial && !measurement.completion.armed();
                        }
                        if (timestamp)
                        {
                            // The pair's preceding GPU use and CPU read have both ended.
                            api.ResetQueries(*query_pool, slot * 2, 2);
                            api.CmdEndQuery(command, *query_pool, slot * 2);
                        }
                        for (auto* entry : batch_entries_)
                        {
                            for (const auto& fill : entry->fills)
                            {
                                const auto access = bufferAccess(api.GetBufferDesc(*fill.target).usage);
                                bufferBarrier(
                                    api,
                                    command,
                                    *fill.target,
                                    {access, nri::StageBits::ALL},
                                    {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}
                                );
                                api.CmdZeroBuffer(command, *fill.target, fill.offset, fill.size);
                                bufferBarrier(
                                    api,
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
                                        api,
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
                                        api,
                                        command,
                                        *copy.image,
                                        copy.staging,
                                        copy.region,
                                        copy.clone,
                                        copy.row,
                                        copy.rows,
                                        copy.row_pitch,
                                        session.description().memoryAlignment.uploadBufferTextureSlice,
                                        copy.first,
                                        source_queue,
                                        session.queue(),
                                        acquiring,
                                        copy.final
                                    );
                                }
                            }
                        }
                        if (timestamp)
                        {
                            api.CmdEndQuery(command, *query_pool, slot * 2 + 1);
                            if (&queue == &session.queue())
                            {
                                api.CmdCopyQueries(
                                    command,
                                    *query_pool,
                                    slot * 2,
                                    2,
                                    *timestamp_results_,
                                    query_index * query_result_stride_
                                );
                            }
                        }
                        if (&queue == &session.queue())
                        {
                            for (std::uint32_t i = 0; i < graphics_measurement_base_; ++i)
                            {
                                const auto serial = measurements_[i].pending_copy_serial;
                                if (serial && serial <= resolve_through)
                                {
                                    api.CmdCopyQueries(
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
            stats.record_submit_ns += elapsed(started);
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
                acquiring ? ++stats.acquire_submissions : ++stats.copy_submissions;
                for (auto* entry : batch_entries_)
                {
                    if (entry->frame_slot != UINT32_MAX)
                    {
                        session.acceptFrameSubmission(entry->frame_slot, queue);
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
                                (void)state.uploads->measure(state.index, state.bits);
                            }
                        },
                        &measurement
                    );
                };
                if (&queue == &session.queue())
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
                    if (&queue == &session.queue())
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
                    ++stats.gpu_samples_skipped;
                }
                if (!acquiring)
                {
                    for (auto* entry : batch_entries_)
                    {
                        for (auto& copy : entry->copies)
                        {
                            session.staging_arena_->markSubmitted(copy.staging, queue);
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
        session.dispatchFrameWrites();
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
                session.deviceLost() ? RenderResult<void>{cxx::unexpected(nativeError(nri::Result::DEVICE_LOST))}
                : stopping           ? RenderResult<void>{cxx::unexpected(RenderError{kUploadStopped})}
                                     : RenderResult<void>{}
            );
        }
    }

    UploadContext::Task UploadContext::writeBuffers(
        std::span<const BufferWrite> writes,
        std::span<const BufferZero> fills,
        std::uint32_t frame_slot
    ) noexcept
    {
        return write(writes, fills, nullptr, {}, {}, nullptr, frame_slot);
    }

    UploadContext::Task UploadContext::writeImage(
        nri::Texture& target,
        std::span<const std::byte> source,
        std::vector<ImageTransferRegion> regions,
        const nri::Texture* clone
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
        std::span<const BufferZero> fills,
        nri::Texture* image,
        std::span<const std::byte> source,
        std::vector<ImageTransferRegion> regions,
        const nri::Texture* clone,
        std::uint32_t frame_slot,
        std::uint32_t element_bytes,
        std::uint32_t source_stride
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
                                     input.offset > session.api().GetBufferDesc(*input.target).size ||
                                     bytes > session.api().GetBufferDesc(*input.target).size - input.offset;
                if (invalid)
                {
                    co_return cxx::unexpected(RenderError{kDeviceArgument});
                }
                if (bytes > options.pending_bytes - pending_bytes)
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
            const auto& description = session.api().GetTextureDesc(*image);
            const bool invalid = description.sampleNum != 1;
            if (invalid)
            {
                co_return cxx::unexpected(RenderError{kDeviceArgument});
            }
            for (const auto& region : regions)
            {
                if (auto valid = imageRegionBytes(description, region, source.size(), options.chunk_bytes); !valid)
                {
                    co_return cxx::unexpected(valid.error());
                }
                const auto bytes =
                    std::size_t((region.texture.width - 1) / region.block_extent + 1) * region.block_bytes;
                const auto alignment = session.description().memoryAlignment.uploadBufferTextureRow;
                if ((bytes + alignment - 1) / alignment * alignment > options.chunk_bytes)
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
                                 fill.offset > session.api().GetBufferDesc(*fill.target).size ||
                                 fill.size > session.api().GetBufferDesc(*fill.target).size - fill.offset;
            if (invalid)
            {
                co_return cxx::unexpected(RenderError{kDeviceArgument});
            }
        }
        if (!image && writes.empty() && fills.empty())
        {
            co_return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (!reserveBytes(pending_bytes))
        {
            co_return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        cxx::scope_exit budget([&]() noexcept { releaseBytes(pending_bytes); });
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        auto& copy_queue = !image || clone ? session.queue() : *this->copy_queue;
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
                    session.staging_arena_->cancel(copy.staging);
                }
            }
        );
        const auto ranges = image ? regions.size() : buffer_runs;
        copies.reserve(std::min(options.window_regions, std::max(options.staging_pages, ranges)));
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
                const auto row_alignment = session.description().memoryAlignment.uploadBufferTextureRow;
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
                auto range = session.staging_arena_->acquire(
                    std::min(remaining, options.chunk_bytes),
                    region ? row_pitch : 4,
                    frame_slot == UINT32_MAX && session.rendering_
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
                BatchEntry wait{this, &copy_queue, {}};
                wait.waiting_range = true;
                wait.frame_slot = frame_slot;
                if (image)
                {
                    const auto& region = regions[index];
                    const auto bytes =
                        std::size_t((region.texture.width - 1) / region.block_extent + 1) * region.block_bytes;
                    const auto alignment = session.description().memoryAlignment.uploadBufferTextureRow;
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
                ++stats.prepare_windows;
                auto prepared = co_await (
                    stdexec::schedule(cpu) |
                    stdexec::then(
                        [&copies, element_bytes, source_stride]() noexcept
                        {
                            const auto started = UploadClock::now();
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
                    stats.copied_bytes +=
                        copy.buffer
                            ? copy.bytes
                            : copy.rows * std::size_t((copy.region.texture.width - 1) / copy.region.block_extent + 1) *
                                  copy.region.block_bytes;
                    if (auto flushed = session.staging_arena_->flush(copy.staging); !flushed)
                    {
                        co_return failure(flushed.error());
                    }
                }
            }
            if (stop.stop_requested())
            {
                co_return failure(RenderError{kUploadStopped});
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

} // namespace lux::render::detail
