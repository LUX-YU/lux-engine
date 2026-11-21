#include <lux/engine/render/extensions/device/CopyEncoding.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/upload/ImageTransfer.hpp>
#include <lux/engine/render/upload/StagingArena.hpp>
#include <numeric>
#include <type_traits>
#include <variant>

namespace lux::render::detail
{
    using UploadClock = std::chrono::steady_clock;

    static std::uint64_t elapsed(UploadClock::time_point start) noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(UploadClock::now() - start).count();
    }

    namespace
    {
        struct BufferPacking final
        {
            std::uint32_t element = 0, stride = 0;

            [[nodiscard]] std::size_t bytes(std::span<const std::byte> source) const noexcept
            {
                return stride ? ((source.size() - element) / stride + 1) * element : source.size();
            }

            [[nodiscard]] bool contiguous() const noexcept { return !stride || stride == element; }
        };

        struct BufferUpload final
        {
            std::span<const BufferWrite> writes;
            std::span<const BufferZero> fills;
            BufferPacking packing;
            SubmissionFrontiers* progress;
        };

        struct ImageUpload final
        {
            nri::Texture& target;
            std::span<const std::byte> source;
            std::vector<ImageTransferRegion> regions;
        };

        struct CloneUpload final
        {
            ImageUpload patch;
            const nri::Texture& original;
        };

        struct BufferChunk final
        {
            nri::Buffer* target;
            std::uint64_t offset;
            std::span<const BufferWrite> inputs;
            std::size_t input_offset;
            BufferPacking packing;

            void prepare(std::span<std::byte> output) const noexcept
            {
                auto local_offset = input_offset;
                for (const auto& input : inputs)
                {
                    const auto count = std::min(output.size(), packing.bytes(input.source) - local_offset);
                    if (packing.contiguous())
                    {
                        std::memcpy(output.data(), input.source.data() + local_offset, count);
                    }
                    else
                    {
                        auto local = local_offset % packing.element;
                        auto source_offset = (local_offset / packing.element) * packing.stride + local;
                        auto* destination = output.data();
                        auto remaining = count;
                        while (remaining)
                        {
                            const auto bytes = std::min(remaining, packing.element - local);
                            std::memcpy(destination, input.source.data() + source_offset, bytes);
                            destination += bytes;
                            source_offset += packing.stride - local;
                            remaining -= bytes;
                            local = 0;
                        }
                    }
                    output = output.subspan(count);
                    local_offset = 0;
                }
            }
        };

        struct ImageChunk final
        {
            nri::Texture* target;
            const nri::Texture* clone;
            nri::TextureRegionDesc region;
            const std::byte* source;
            std::uint64_t source_pitch;
            std::size_t row_bytes;
            std::uint32_t rows, row_pitch;
            bool first, final;

            void prepare(std::span<std::byte> output) const noexcept
            {
                for (std::uint32_t row = 0; row < rows; ++row)
                    std::memcpy(output.data() + row * row_pitch, source + row * source_pitch, row_bytes);
            }
        };

        using VCopyChunk = std::variant<BufferChunk, ImageChunk>;

        struct StagingRequest final
        {
            std::size_t bytes, unit;
        };

        struct BufferCursor final
        {
            std::span<const BufferWrite> writes;
            BufferPacking packing;
            std::size_t index = 0, offset = 0, run_remaining = 0;

            [[nodiscard]] bool done() const noexcept { return index == writes.size(); }

            [[nodiscard]] StagingRequest next() noexcept
            {
                if (!run_remaining)
                {
                    const auto bytes = packing.bytes(writes[index].source);
                    run_remaining = bytes - offset;
                    auto end = writes[index].offset + bytes;
                    for (auto next = index + 1; next < writes.size(); ++next)
                    {
                        if (writes[next].target != writes[index].target || writes[next].offset != end) break;
                        const auto bytes = packing.bytes(writes[next].source);
                        run_remaining += bytes;
                        end += bytes;
                    }
                }
                return {run_remaining, 4};
            }

            [[nodiscard]] VCopyChunk lower(std::size_t bytes, bool) noexcept
            {
                BufferChunk chunk{writes[index].target, writes[index].offset + offset, {}, offset, packing};
                const auto first = index;
                auto remaining = bytes;
                while (remaining)
                {
                    const auto input_bytes = packing.bytes(writes[index].source);
                    const auto part = std::min(remaining, input_bytes - offset);
                    remaining -= part;
                    offset += part;
                    if (offset == input_bytes)
                    {
                        ++index;
                        offset = 0;
                    }
                }
                chunk.inputs = writes.subspan(first, index - first + (offset != 0));
                run_remaining -= bytes;
                return chunk;
            }
        };

        [[nodiscard]] std::size_t blockRowBytes(const ImageTransferRegion& region) noexcept
        {
            return std::size_t((region.texture.width - 1) / region.block_extent + 1) * region.block_bytes;
        }

        [[nodiscard]] std::size_t alignedRowBytes(std::size_t bytes, std::uint32_t alignment) noexcept
        {
            return (bytes + alignment - 1) / alignment * alignment;
        }

        struct ImageCursor final
        {
            const ImageUpload* upload;
            const nri::Texture* clone;
            std::uint32_t alignment;
            std::size_t index = 0;
            std::uint32_t row = 0;

            [[nodiscard]] bool done() const noexcept { return index == upload->regions.size(); }

            [[nodiscard]] StagingRequest next() const noexcept
            {
                const auto& region = upload->regions[index];
                const auto block_rows = (region.texture.height - 1) / region.block_extent + 1;
                const auto pitch = alignedRowBytes(blockRowBytes(region), alignment);
                return {std::size_t(block_rows - row) * pitch, pitch};
            }

            [[nodiscard]] VCopyChunk lower(std::size_t bytes, bool first) noexcept
            {
                const auto& input = upload->regions[index];
                const auto range = next();
                const auto rows = static_cast<std::uint32_t>(bytes / range.unit);
                const bool complete_region = bytes == range.bytes;
                auto region = input.texture;
                region.y += row * input.block_extent;
                region.height = std::min(
                    rows * input.block_extent, std::uint32_t(input.texture.height - row * input.block_extent)
                );
                ImageChunk chunk{
                    &upload->target, first ? clone : nullptr, region,
                    upload->source.data() + input.source_offset + row * input.source_row_pitch, input.source_row_pitch,
                    blockRowBytes(input), rows, static_cast<std::uint32_t>(range.unit), first,
                    complete_region && index + 1 == upload->regions.size()
                };
                row += rows;
                if (complete_region)
                {
                    ++index;
                    row = 0;
                }
                return chunk;
            }
        };

        struct UploadPlan final
        {
            std::variant<BufferCursor, ImageCursor> cursor;
            std::size_t source_bytes, region_count;
            SubmissionQueue* queue;
            std::span<const BufferZero> fills;
            SubmissionFrontiers* progress;
        };

        [[nodiscard]] RenderResult<UploadPlan>
        normalizeUpload(const BufferUpload& upload, DeviceSession& device, const TransferOptions& options) noexcept
        {
            const auto packing = upload.packing;
            const bool strided = packing.element || packing.stride;
            const bool invalid_layout = strided && (!packing.element || packing.element % 4 ||
                packing.stride < packing.element);
            if (invalid_layout || (upload.writes.empty() && upload.fills.empty()))
                return cxx::unexpected(RenderError{kDeviceArgument});
            std::size_t pending = 0, runs = 0;
            const nri::Buffer* previous = nullptr;
            std::uint64_t end = 0;
            const auto& core = device.nriCore();
            for (const auto& input : upload.writes)
            {
                const auto size = input.source.size();
                const bool invalid_source = strided ? size < packing.element ||
                    (size - packing.element) % packing.stride != 0 : !size;
                if (!input.target || invalid_source) return cxx::unexpected(RenderError{kDeviceArgument});
                const auto bytes = packing.bytes(input.source);
                const auto capacity = core.GetBufferDesc(*input.target).size;
                const bool invalid_range = input.offset > capacity || bytes > capacity - input.offset;
                if (invalid_range || (bytes | input.offset) % 4)
                    return cxx::unexpected(RenderError{kDeviceArgument});
                if (bytes > options.pending_bytes - pending) return cxx::unexpected(RenderError{kDeviceCapacity});
                pending += bytes;
                runs += input.target != previous || input.offset != end;
                previous = input.target;
                end = input.offset + bytes;
            }
            for (const auto& fill : upload.fills)
            {
                if (!fill.target) return cxx::unexpected(RenderError{kDeviceArgument});
                const auto capacity = core.GetBufferDesc(*fill.target).size;
                const bool invalid_range = !fill.size || fill.offset > capacity || fill.size > capacity - fill.offset;
                if (invalid_range || (fill.offset | fill.size) % 4)
                    return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return UploadPlan{BufferCursor{upload.writes, packing}, pending, runs, &device.queue(),
                upload.fills, upload.progress};
        }

        [[nodiscard]] RenderResult<UploadPlan> normalizeImageUpload(
            const ImageUpload& upload, const nri::Texture* clone, SubmissionQueue& queue, DeviceSession& device,
            const TransferOptions& options
        ) noexcept
        {
            const auto& description = device.nriCore().GetTextureDesc(upload.target);
            if (upload.source.empty() || upload.regions.empty() || description.sampleNum != 1)
                return cxx::unexpected(RenderError{kDeviceArgument});
            const auto alignment = device.description().memoryAlignment.uploadBufferTextureRow;
            for (const auto& region : upload.regions)
            {
                auto valid = imageRegionBytes(description, region, upload.source.size(), options.chunk_bytes);
                if (!valid) return cxx::unexpected(valid.error());
                if (alignedRowBytes(blockRowBytes(region), alignment) > options.chunk_bytes)
                    return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return UploadPlan{ImageCursor{&upload, clone, alignment}, upload.source.size(), upload.regions.size(),
                &queue, {}, nullptr};
        }
    } // namespace

    struct ResourceUploader::UploadRequest final
    {
        std::variant<BufferUpload, ImageUpload, CloneUpload> value;
    };

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
        VCopyChunk chunk;

        [[nodiscard]] bool needsAcquire() const noexcept
        {
            const auto* image = std::get_if<ImageChunk>(&chunk);
            return image && image->final;
        }

        [[nodiscard]] std::size_t sourceBytes() const noexcept
        {
            if (const auto* image = std::get_if<ImageChunk>(&chunk)) return image->rows * image->row_bytes;
            return staging.bytes.size();
        }
    };

    struct ResourceUploader::FrameCopy final
    {
        StagingRange staging;
        nri::Buffer* buffer;
        std::uint64_t offset;
    };

    struct ResourceUploader::UploadWindow
    {
        enum class EPhase : std::uint8_t { WAIT_RANGE, COPY, ACQUIRE, COMPLETE };

        struct Request final
        {
            std::span<Copy> copies{};
            std::span<const BufferZero> fills{};
            SubmissionFrontiers* progress = nullptr;
            std::size_t minimum_range = 0;
        };

        struct Receipt final
        {
            SubmissionPoint copy, acquire;
            RenderResult<void> result;
        };

        UploadWindow(ResourceUploader& owner, SubmissionQueue& source, Request input) noexcept
            : uploads(owner), queue(&source), request(input),
              phase(input.minimum_range ? EPhase::WAIT_RANGE : EPhase::COPY)
        {
        }

        void deliver(RenderResult<void> result) noexcept
        {
            if (next || phase == EPhase::COMPLETE) std::terminate();
            phase = EPhase::COMPLETE;
            receipt.result = std::move(result);
            complete(*this);
        }

        ResourceUploader& uploads;
        SubmissionQueue* queue;
        Request request;
        EPhase phase;
        Receipt receipt{};
        UploadWindow* next = nullptr;
        bool (*stopping)(UploadWindow&) noexcept = nullptr;
        void (*complete)(UploadWindow&) noexcept = nullptr;
    };

    class ResourceUploader::WindowSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(UploadWindow::Receipt)>;

        WindowSender(ResourceUploader& uploads, SubmissionQueue& queue, UploadWindow::Request request) noexcept
            : uploads_(&uploads), queue_(&queue), request_(request)
        {
        }

        template <class Receiver> struct Operation final : UploadWindow
        {
            using operation_state_concept = stdexec::operation_state_t;

            Operation(ResourceUploader& uploads, SubmissionQueue& queue, Request request, Receiver receiver) noexcept
                : UploadWindow(uploads, queue, request), receiver_(std::move(receiver))
            {
            }

            void start() & noexcept
            {
                stopping = [](UploadWindow& value) noexcept
                {
                    auto& self = static_cast<Operation&>(value);
                    return stdexec::get_stop_token(stdexec::get_env(self.receiver_)).stop_requested();
                };
                complete = [](UploadWindow& value) noexcept
                {
                    auto& self = static_cast<Operation&>(value);
                    auto delivered = std::move(self.receipt);
                    stdexec::set_value(std::move(self.receiver_), std::move(delivered));
                };
                uploads.enqueue(*this);
            }

            Receiver receiver_;
        };

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            return Operation<Receiver>{*uploads_, *queue_, request_, std::move(receiver)};
        }

    private:
        ResourceUploader* uploads_;
        SubmissionQueue* queue_;
        UploadWindow::Request request_;
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
        if (hasPendingTransfers() || frame_copy_receipt_serial_)
        {
            std::terminate();
        }
        for (auto& copy : frame_copies_)
        {
            staging_arena_->cancel(copy.staging);
        }
        for (std::size_t index = 0; index < command_windows_.size(); ++index)
        {
            auto& window = command_windows_[index];
            const auto& queue = device_.physicalQueue(index / options_.staging_pages);
            const bool has_live_gpu_use = window.serial > queue.completed() && !queue.idle() && !queue.deviceLost();
            if (has_live_gpu_use) std::terminate();
            device_.nriCore().DestroyCommandBuffer(window.command);
            device_.nriCore().DestroyCommandAllocator(window.allocator);
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
        state->command_windows_.resize(device_.queueCount() * options_.staging_pages);
        state->next_commands_.resize(device_.queueCount());
        for (std::size_t q = 0; q < device_.queueCount(); ++q)
        {
            for (std::size_t slot = 0; slot < options_.staging_pages; ++slot)
            {
                auto& window = state->command_windows_[q * options_.staging_pages + slot];
                auto result =
                    device_.nriCore().CreateCommandAllocator(device_.physicalQueue(q).native(), window.allocator);
                if (result == nri::Result::SUCCESS)
                {
                    result = device_.nriCore().CreateCommandBuffer(*window.allocator, window.command);
                }
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
        }
        state->copy_timestamp_bits_ = state->copy_queue_->properties().timestamp_bits;
        state->graphics_timestamp_bits_ = device_.queue().properties().timestamp_bits;
        if (options_.measure_gpu && (state->copy_timestamp_bits_ || state->graphics_timestamp_bits_))
        {
            state->graphics_measurement_base_ = state->copy_queue_ == &device_.queue() ? 0 : options_.staging_pages;
            const auto query_slots = state->graphics_measurement_base_ + options_.staging_pages;
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
                const nri::QueryPoolDesc query{type, static_cast<std::uint32_t>(options_.staging_pages) * 2};
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
        const nri::CoreInterface& core, nri::CommandBuffer& command, const StagingRange& staging,
        const BufferChunk& chunk
    ) noexcept
    {
        auto& target = *chunk.target;
        const nri::AccessStage access{bufferAccess(core.GetBufferDesc(target).usage), nri::StageBits::ALL};
        const nri::AccessStage copy{nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        bufferBarrier(core, command, target, access, copy);
        recordCopy(core, command, BufferCopyRegion{
            staging.buffer, &target, staging.offset, chunk.offset, staging.bytes.size()
        });
        bufferBarrier(core, command, target, copy, access);
    }

    struct ImageHandoff final
    {
        SubmissionQueue& source;
        SubmissionQueue& graphics;
        nri::GraphicsAPI backend;
        std::uint32_t slice_alignment;
        bool acquiring;
    };

    static void recordImage(
        const nri::CoreInterface& nri_core, nri::CommandBuffer& command, const StagingRange& range,
        const ImageChunk& chunk, const ImageHandoff& handoff
    ) noexcept
    {
        auto& target = *chunk.target;
        auto& copy = handoff.source;
        auto& graphics = handoff.graphics;
        const bool acquiring = handoff.acquiring;
        const bool distinct = &copy != &graphics;
        const bool common_handoff = distinct && handoff.backend == nri::GraphicsAPI::D3D12 &&
            copy.properties().type == nri::QueueType::COPY;
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
            if (chunk.first)
            {
                barrier.before = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::NONE};
                barrier
                    .after = {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
                emit(barrier);
                if (chunk.clone)
                {
                    auto original = barrier;
                    original.texture = const_cast<nri::Texture*>(chunk.clone);
                    original
                        .before = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::ALL};
                    original.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
                    emit(original);
                    nri::TextureRegionDesc region{};
                    region.width = description.width;
                    region.height = description.height;
                    region.depth = 1;
                    region.planes = nri::PlaneBits::COLOR;
                    recordCopy(nri_core, command, TextureCopyRegion{
                        chunk.clone, &target, region, region, description.mipNum, description.layerNum
                    });
                    std::swap(original.before, original.after);
                    emit(original);
                    barrier.before = barrier.after;
                    emit(barrier); // Clone writes precede region patch writes.
                }
            }
            const auto slice_unit = std::lcm(chunk.row_pitch, handoff.slice_alignment);
            const auto slice_pitch = ((chunk.row_pitch * chunk.rows + slice_unit - 1) / slice_unit) * slice_unit;
            const nri::TextureDataLayoutDesc layout{range.offset, chunk.row_pitch, slice_pitch};
            recordUpload(nri_core, command, target, chunk.region, *range.buffer, layout);
        }
        if (chunk.final)
        {
            // D3D12 COPY lists remain in COMMON (NRI GENERAL). The existing fence dependency
            // orders the Graphics acquire, which alone transitions to SHADER_RESOURCE.
            barrier.before = {
                acquiring ? nri::AccessBits::NONE : nri::AccessBits::COPY_DESTINATION,
                acquiring && common_handoff ? nri::Layout::GENERAL
                    : acquiring && !cross_family ? nri::Layout::SHADER_RESOURCE : nri::Layout::COPY_DESTINATION,
                acquiring ? nri::StageBits::NONE : nri::StageBits::COPY
            };
            barrier.after = {
                !distinct || acquiring ? nri::AccessBits::SHADER_RESOURCE : nri::AccessBits::NONE,
                common_handoff && !acquiring ? nri::Layout::GENERAL : nri::Layout::SHADER_RESOURCE,
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
        if (!write.target || write.source.empty())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (!staging_arena_->canAcquireWhole(write.source.size(), unit, options_.window_regions - frame_copies_.size()))
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        auto staged = stageBufferPrefix(write, unit);
        if (!staged || *staged != write.source.size())
        {
            std::terminate(); // Preflight and consumption are one control-owner segment.
        }
        return staged;
    }

    RenderResult<std::size_t> ResourceUploader::stageBufferPrefix(const BufferWrite& write, std::size_t unit) noexcept
    {
        const bool invalid_range = unit == 0 || write.source.size() % unit != 0;
        if (!write.target || write.source.empty() || invalid_range)
            return cxx::unexpected(RenderError{kDeviceArgument});

        std::size_t offset = 0;
        while (offset < write.source.size() && frame_copies_.size() < options_.window_regions)
        {
            auto range = staging_arena_->acquire(write.source.size() - offset, unit);
            if (!range) break;
            const auto bytes = range->bytes.size();
            std::memcpy(range->bytes.data(), write.source.data() + offset, bytes);
            frame_copies_.push_back({std::move(*range), write.target, write.offset + offset});
            offset += bytes;
        }
        return offset;
    }

    RenderResult<void> ResourceUploader::flushFrameCopies() noexcept
    {
        for (auto& copy : frame_copies_)
        {
            if (auto flushed = staging_arena_->flush(copy.staging); !flushed)
            {
                return flushed;
            }
            statistics_.copied_bytes += copy.staging.bytes.size();
        }
        return {};
    }

    bool ResourceUploader::hasFrameCopies() const noexcept
    {
        return stagedFrameCopyCount() != 0;
    }

    std::size_t ResourceUploader::stagedFrameCopyCount() const noexcept
    {
        return frame_copies_.size();
    }

    void ResourceUploader::recordFrameCopies(nri::CommandBuffer& command) noexcept
    {
        const auto& nri_core = device_.nriCore();
        std::size_t first = 0;
        while (first < frame_copies_.size())
        {
            auto* target = frame_copies_[first].buffer;
            auto end = frame_copies_[first].offset + frame_copies_[first].staging.bytes.size();
            auto last = first + 1;
            while (last < frame_copies_.size())
            {
                const auto& next = frame_copies_[last];
                if (next.buffer != target || next.offset < end)
                {
                    break;
                }
                end = next.offset + next.staging.bytes.size();
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
            for (auto i = first; i < last;)
            {
                const auto& copy = frame_copies_[i];
                auto bytes = copy.staging.bytes.size();
                auto next = i + 1;
                while (next < last)
                {
                    const auto& following = frame_copies_[next];
                    const bool adjacent = following.offset == copy.offset + bytes &&
                        following.staging.buffer == copy.staging.buffer &&
                        following.staging.offset == copy.staging.offset + bytes;
                    if (!adjacent) break;
                    bytes += following.staging.bytes.size();
                    ++next;
                }
                // Merge only the native copy. Every separately acquired range keeps its existing
                // commit/cancel responsibility and no span is formed across borrowed CPU objects.
                recordCopy(nri_core, command, BufferCopyRegion{
                    copy.staging.buffer, target, copy.staging.offset, copy.offset, bytes
                });
                i = next;
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

    void ResourceUploader::acceptFrameCopies(FrameCopyReceipt receipt) noexcept
    {
        const auto queue_index = device_.queueIndex(device_.queue());
        const bool wrong_submit = receipt.point.domain != &device_ || receipt.point.queue != queue_index ||
            receipt.point.serial != frame_copy_receipt_serial_;
        const bool wrong_ranges = !receipt.ranges || receipt.ranges != frame_copies_.size();
        if (wrong_submit || wrong_ranges) std::terminate();
        for (auto& copy : frame_copies_)
        {
            staging_arena_->markSubmitted(copy.staging, device_.queue(), receipt.point.serial);
            ++statistics_.copy_regions;
        }
        frame_copies_.clear();
        frame_copy_receipt_serial_ = 0;
    }

    void ResourceUploader::cancelFrameCopies() noexcept
    {
        if (frame_copy_receipt_serial_) std::terminate();
        // UNKNOWN returns only after the native queue has retired the uncertain submission.
        for (auto& copy : frame_copies_) staging_arena_->cancel(copy.staging);
        frame_copies_.clear();
    }

    void ResourceUploader::enqueue(UploadWindow& entry) noexcept
    {
        // Current-frame writers precede background range waiters in the same finite list.
        // Pending preparation can reserve one existing page. Both uses share the Arena
        // and waiter list, and transfers can use every page after preparation settles.
        if (entry.phase == UploadWindow::EPhase::WAIT_RANGE && entry.request.progress)
        {
            entry.next = range_waiters_;
            range_waiters_ = &entry;
            if (!range_waiters_tail_)
            {
                range_waiters_tail_ = &entry;
            }
            return;
        }
        const bool waiting_range = entry.phase == UploadWindow::EPhase::WAIT_RANGE;
        auto*& head = waiting_range ? range_waiters_ : ready_head_;
        auto*& tail = waiting_range ? range_waiters_tail_ : ready_tail_;
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

    RenderResult<nri::CommandBuffer*>
    ResourceUploader::beginCommands(SubmissionQueue& queue, std::uint32_t& slot) noexcept
    {
        if (const auto error = queue.terminalError(); error.type)
        {
            return cxx::unexpected(error);
        }
        if (queue.submitting())
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        const auto index = device_.queueIndex(queue);
        slot = next_commands_[index];
        auto& window = command_windows_[index * options_.staging_pages + slot];
        if (window.serial > queue.completed())
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        auto result = device_.resetCommandAllocator(*window.allocator);
        if (result == nri::Result::SUCCESS)
        {
            result = device_.nriCore().BeginCommandBuffer(*window.command, nullptr);
        }
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return window.command;
    }

    void ResourceUploader::acceptCommands(SubmissionQueue& queue, std::uint32_t slot, std::uint64_t serial) noexcept
    {
        const auto index = device_.queueIndex(queue);
        command_windows_[index * options_.staging_pages + slot].serial = serial;
        next_commands_[index] = (slot + 1) % options_.staging_pages;
    }

    exec::task<RenderResult<std::optional<ResourceUploader::FrameCopyReceipt>>>
    ResourceUploader::submitPreparationCopies() noexcept
    {
        if (frame_copy_receipt_serial_) std::terminate();
        const auto ranges = stagedFrameCopyCount();
        if (!ranges) co_return std::nullopt;
        auto& graphics = device_.queue();
        std::uint32_t slot = 0;
        auto recorded = beginCommands(graphics, slot);
        if (!recorded)
        {
            const auto error = recorded.error();
            if (error.type == kDeviceBusy || error.type == kDeviceCapacity) co_return std::nullopt;
            co_return cxx::unexpected(error);
        }
        auto* command = *recorded;
        recordFrameCopies(*command);
        const auto ended = device_.nriCore().EndCommandBuffer(*command);
        if (ended != nri::Result::SUCCESS) co_return cxx::unexpected(nativeError(ended));
        auto accepted = co_await graphics.submit({&command, 1});
        if (!accepted) co_return cxx::unexpected(accepted.error());
        acceptCommands(graphics, slot, *accepted);
        ++statistics_.copy_submissions;
        frame_copy_receipt_serial_ = *accepted;
        co_return FrameCopyReceipt{device_.submissionPoint(device_.queueIndex(graphics), *accepted), ranges};
    }

    bool ResourceUploader::canBeginCommands(const SubmissionQueue& queue) const noexcept
    {
        const auto index = device_.queueIndex(queue);
        const auto& window = command_windows_[index * options_.staging_pages + next_commands_[index]];
        return queue.terminalError().type || (!queue.submitting() && window.serial <= queue.completed());
    }

    bool ResourceUploader::canSubmitPreparationCopies() const noexcept
    {
        return canBeginCommands(device_.queue());
    }

    bool ResourceUploader::canProgressTransfers() const noexcept
    {
        if (progressing_)
        {
            return false;
        }
        if (ready_head_)
        {
            const bool acquiring = ready_head_->phase == UploadWindow::EPhase::ACQUIRE;
            const auto& queue = acquiring ? device_.queue() : *ready_head_->queue;
            if (canBeginCommands(queue)) return true;
        }
        if (!range_waiters_) return false;
        const bool is_stopped = range_waiters_->stopping(*range_waiters_);
        const bool has_terminal = device_.terminalError().type != 0;
        if (is_stopped || has_terminal) return true;
        const bool reserve_frame = !range_waiters_->request.progress && preparation_reserved_;
        return staging_arena_->canAcquire(range_waiters_->request.minimum_range, reserve_frame);
    }

    ResourceUploader::Task ResourceUploader::submitReadyTransfers() noexcept
    {
        if (progressing_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        progressing_ = true;
        cxx::scope_exit release([this]() noexcept { progressing_ = false; });
        // Complete only this bounded prefix. Receiver reentry may append work for the next flush.
        auto* boundary = ready_tail_;
        UploadWindow* blocked_head = nullptr;
        UploadWindow* blocked_tail = nullptr;
        while (ready_head_ && boundary)
        {
            auto& first = *ready_head_;
            const bool acquiring = first.phase == UploadWindow::EPhase::ACQUIRE;
            auto& source_queue = *first.queue;
            auto& queue = acquiring ? device_.queue() : source_queue;
            batch_entries_.clear();
            while (ready_head_ && batch_entries_.size() < options_.batch_entries)
            {
                auto* entry = ready_head_;
                const bool same_submission = entry->queue == &source_queue && entry->phase == first.phase;
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
            std::uint64_t accepted_serial = 0;
            nri::FenceSubmitDesc wait{};
            if (acquiring)
            {
                std::uint64_t producer = 0;
                for (const auto* entry : batch_entries_)
                {
                    producer = std::max(producer, entry->receipt.copy.serial);
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
                std::uint32_t command_slot = 0;
                auto commands = beginCommands(queue, command_slot);
                const auto record = [&](nri::CommandBuffer& command, std::uint32_t slot) noexcept -> RenderResult<void>
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
                    const ImageHandoff handoff{
                        source_queue, device_.queue(), device_.description().graphicsAPI,
                        device_.description().memoryAlignment.uploadBufferTextureSlice, acquiring
                    };
                    for (auto* entry : batch_entries_)
                    {
                        for (const auto& fill : entry->request.fills)
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
                        for (auto& copy : entry->request.copies)
                        {
                            if (const auto* image = std::get_if<ImageChunk>(&copy.chunk))
                            {
                                if (!acquiring || image->final)
                                    recordImage(nri_core, command, copy.staging, *image, handoff);
                            }
                            else
                            {
                                if (acquiring) std::terminate();
                                recordBuffer(nri_core, command, copy.staging, std::get<BufferChunk>(copy.chunk));
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
                };
                if (!commands)
                {
                    accepted = cxx::unexpected(commands.error());
                }
                else
                {
                    accepted = record(**commands, command_slot);
                    const auto ended = device_.nriCore().EndCommandBuffer(**commands);
                    if (ended != nri::Result::SUCCESS)
                    {
                        accepted = cxx::unexpected(nativeError(ended));
                    }
                    if (accepted)
                    {
                        auto submitted = co_await queue.submit(
                            {&*commands, 1},
                            acquiring ? std::span<const nri::FenceSubmitDesc>(&wait, 1)
                                      : std::span<const nri::FenceSubmitDesc>{}
                        );
                        if (submitted)
                        {
                            accepted_serial = *submitted;
                            acceptCommands(queue, command_slot, accepted_serial);
                        }
                        else
                        {
                            accepted = cxx::unexpected(submitted.error());
                        }
                    }
                }
            }
            if (options_.measure_cpu)
            {
                statistics_.record_submit_ns += elapsed(started);
            }
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
                const auto point = device_.submissionPoint(device_.queueIndex(queue), accepted_serial);
                for (auto* entry : batch_entries_)
                {
                    (acquiring ? entry->receipt.acquire : entry->receipt.copy) = point;
                    if (entry->request.progress) entry->request.progress->accept(point);
                }
                const auto observe_measurement = [&](Measurement& measurement) noexcept
                {
                    queue.arm(
                        accepted_serial,
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
                        measurement.pending_copy_serial = accepted_serial;
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
                        for (auto& copy : entry->request.copies)
                        {
                            staging_arena_->markSubmitted(copy.staging, queue, accepted_serial);
                            ++statistics_.copy_regions;
                        }
                    }
                }
            }
            for (auto* entry : batch_entries_)
            {
                const bool needs_acquire =
                    accepted && !acquiring && entry->queue != &device_.queue() &&
                    std::ranges::any_of(entry->request.copies, [](const auto& copy) { return copy.needsAcquire(); });
                if (needs_acquire)
                {
                    entry->phase = UploadWindow::EPhase::ACQUIRE;
                    enqueue(*entry);
                }
                else
                {
                    entry->deliver(accepted);
                    // Delivery can destroy this operation or enqueue a new window.
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
            const bool stopping = entry->stopping(*entry);
            const bool reserve_frame = !entry->request.progress && preparation_reserved_;
            if (!stopping && !device_.terminalError().type &&
                !staging_arena_->canAcquire(entry->request.minimum_range, reserve_frame))
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
            entry->deliver(
                device_.terminalError().type ? RenderResult<void>{cxx::unexpected(device_.terminalError())}
                : stopping                   ? RenderResult<void>{cxx::unexpected(RenderError{kUploadStopped})}
                                             : RenderResult<void>{}
            );
        }
        co_return RenderResult<void>{};
    }

    ResourceUploader::Task ResourceUploader::writeBuffers(
        std::span<const BufferWrite> writes,
        std::span<const BufferZero> fills,
        SubmissionFrontiers* progress
    ) noexcept
    {
        return write({BufferUpload{writes, fills, {}, progress}});
    }

    ResourceUploader::Task ResourceUploader::writeBuffers(
        std::span<const BufferWrite> writes,
        std::uint32_t element_bytes,
        std::uint32_t source_stride,
        SubmissionFrontiers* progress
    ) noexcept
    {
        return write({BufferUpload{writes, {}, {element_bytes, source_stride}, progress}});
    }

    ResourceUploader::Task ResourceUploader::writeImage(
        nri::Texture& target, std::span<const std::byte> source, std::vector<ImageTransferRegion> regions
    ) noexcept
    {
        return write({ImageUpload{target, source, std::move(regions)}});
    }

    ResourceUploader::Task ResourceUploader::cloneImage(
        nri::Texture& target, const nri::Texture& original, std::span<const std::byte> source,
        std::vector<ImageTransferRegion> regions
    ) noexcept
    {
        return write({CloneUpload{{target, source, std::move(regions)}, original}});
    }

    ResourceUploader::Task ResourceUploader::write(UploadRequest request) noexcept
    {
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        auto plan = std::visit([&](const auto& input) noexcept
        {
            using Input = std::remove_cvref_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, BufferUpload>) return normalizeUpload(input, device_, options_);
            if constexpr (std::is_same_v<Input, ImageUpload>)
                return normalizeImageUpload(input, nullptr, *copy_queue_, device_, options_);
            if constexpr (std::is_same_v<Input, CloneUpload>)
                return normalizeImageUpload(input.patch, &input.original, device_.queue(), device_, options_);
        }, request.value);
        if (!plan) co_return cxx::unexpected(plan.error());
        const auto source_bytes = plan->source_bytes;
        if (!reserveSourceBytes(source_bytes)) co_return cxx::unexpected(RenderError{kDeviceCapacity});
        cxx::scope_exit budget([&]() noexcept { releaseSourceBytes(source_bytes); });

        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        bool accepted_any = false;
        const auto failure = [&](RenderError error) noexcept
        {
            const bool lost = isNativeFailure(error, nri::Result::DEVICE_LOST);
            return cxx::unexpected(accepted_any && lost ? RenderError{kOutcomeUnknown} : error);
        };
        std::vector<Copy> copies;
        cxx::scope_exit release_ranges([&]() noexcept
        {
            for (auto& copy : copies) staging_arena_->cancel(copy.staging);
        });
        const auto window_limit = std::min(options_.window_regions, std::max(options_.staging_pages, plan->region_count));
        copies.reserve(window_limit);
        const auto* buffer = std::get_if<BufferCursor>(&plan->cursor);
        const bool contiguous = buffer && buffer->packing.contiguous();
        const auto done = [&]() noexcept
        {
            return std::visit([](const auto& cursor) noexcept { return cursor.done(); }, plan->cursor);
        };
        while (!done() || !accepted_any)
        {
            if (stop.stop_requested()) co_return failure(RenderError{kUploadStopped});
            copies.clear();
            std::size_t minimum_range = 0;
            while (!done() && copies.size() < window_limit)
            {
                const auto next = std::visit([](auto& cursor) noexcept { return cursor.next(); }, plan->cursor);
                auto range = staging_arena_->acquire(
                    std::min(next.bytes, options_.chunk_bytes), next.unit, !plan->progress && preparation_reserved_
                );
                if (!range)
                {
                    minimum_range = next.unit;
                    break;
                }
                const bool first = !accepted_any && copies.empty();
                auto chunk = std::visit([&](auto& cursor) noexcept
                {
                    return cursor.lower(range->bytes.size(), first);
                }, plan->cursor);
                copies.push_back({std::move(*range), std::move(chunk)});
            }
            if (copies.empty() && minimum_range)
            {
                const auto available = co_await WindowSender{
                    *this, *plan->queue, {.progress = plan->progress, .minimum_range = minimum_range}
                };
                if (!available.result) co_return failure(available.result.error());
                continue;
            }
            if (!copies.empty())
            {
                ++statistics_.prepare_windows;
                const auto prepare_window = [&copies, measure = options_.measure_cpu]() noexcept
                {
                    const auto started = measure ? UploadClock::now() : UploadClock::time_point{};
                    for (auto& copy : copies)
                    {
                        std::visit([&](const auto& chunk) noexcept { chunk.prepare(copy.staging.bytes); }, copy.chunk);
                    }
                    return RenderResult<std::uint64_t>{measure ? elapsed(started) : 0};
                };
                std::size_t window_bytes = 0;
                for (const auto& copy : copies) window_bytes += copy.staging.bytes.size();
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
                        ) |
                        stdexec::continues_on(owner) | stdexec::upon_error(NativeDeliveryFailure<std::uint64_t>{})
                    );
                }
                if (!prepared) co_return failure(prepared.error());
                statistics_.prepare_ns += *prepared;
                for (const auto& copy : copies)
                {
                    statistics_.copied_bytes += copy.sourceBytes();
                    if (auto flushed = staging_arena_->flush(copy.staging); !flushed)
                        co_return failure(flushed.error());
                }
            }
            if (stop.stop_requested()) co_return failure(RenderError{kUploadStopped});
            const auto fills = accepted_any ? std::span<const BufferZero>{} : plan->fills;
            const auto submitted = co_await WindowSender{*this, *plan->queue, {copies, fills, plan->progress}};
            if (!submitted.result)
            {
                // A rejected Acquire still leaves the accepted Copy in its physical queue's frontier.
                accepted_any |= submitted.copy.serial != 0;
                co_return failure(submitted.result.error());
            }
            accepted_any = true;
        }
        co_return RenderResult<void>{};
    }

} // namespace lux::render::detail
