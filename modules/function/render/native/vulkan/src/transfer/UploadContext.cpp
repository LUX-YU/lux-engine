#include "Native.hpp"
#include "StagingArena.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

namespace lux::render::vulkan
{
    using UploadClock = std::chrono::steady_clock;

    static std::uint64_t elapsed(UploadClock::time_point start) noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(UploadClock::now() - start).count();
    }

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
            vkDestroyQueryPool(session.device().native(), timestamps, nullptr);
        }
    }

    RenderResult<void> DeviceSession::configureUploads(
        const TransferOptions& options,
        execution::CpuScheduler cpu
    ) noexcept
    {
        const bool invalid = uploads_ || cpu == execution::CpuScheduler{} || options.chunk_bytes < 256 ||
                             options.chunk_bytes > 16 * 1024 * 1024 || options.staging_pages == 0 ||
                             options.staging_pages > 16 || options.pending_bytes < options.chunk_bytes;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (options.measure_gpu && !device_.hostQueryReset())
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        auto state = std::unique_ptr<UploadContext>(new UploadContext(*this, options, cpu));
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device_.physical(), &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families_info(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device_.physical(), &family_count, families_info.data());
        auto& transfer = queue(EQueueRole::TRANSFER);
        const auto granularity = families_info[transfer.nativeQueue().family].minImageTransferGranularity;
        // This row-chunk algorithm requires independently transferable texel/block rows. Other hardware
        // uses the existing Graphics queue; do not emit invalid sub-image transfers on restrictive families.
        const bool row_chunks = granularity.width == 1 && granularity.height == 1 && granularity.depth == 1;
        state->copy_queue = row_chunks ? &transfer : &queue();
        state->copy_timestamp_bits = families_info[state->copy_queue->nativeQueue().family].timestampValidBits;
        state->graphics_timestamp_bits = families_info[queue().nativeQueue().family].timestampValidBits;
        if (options.measure_gpu && (state->copy_timestamp_bits || state->graphics_timestamp_bits))
        {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = std::uint32_t(options.staging_pages * 2);
            const auto result = vkCreateQueryPool(device_.native(), &query, nullptr, &state->timestamps);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        const std::array families{queue().nativeQueue().family, state->copy_queue->nativeQueue().family};
        const auto sharing = families[0] == families[1] ? std::span<const std::uint32_t>{} : std::span(families);
        auto arena = StagingArena::create(allocator_, options.chunk_bytes, options.staging_pages, sharing);
        if (!arena)
        {
            return cxx::unexpected(arena.error());
        }
        staging_arena_ = std::move(*arena);
        state->stats.staging_bytes = options.chunk_bytes * options.staging_pages;
        uploads_ = std::move(state);
        return {};
    }

    TransferStats DeviceSession::uploadStats() const noexcept
    {
        return uploads_ ? uploads_->stats : TransferStats{};
    }

    static void recordBuffer(
        VkCommandBuffer command,
        Buffer& target,
        StagingPage& page,
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
            const VkBufferCopy copy{0, offset, chunk_size};
            vkCmdCopyBuffer(command, page.buffer().native(), target.native(), 1, &copy);
        }
        if (!final)
        {
            return;
        }
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.buffer = target.native();
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
        Image& target,
        StagingPage& page,
        const ImageTransferRegion& input,
        const Image* clone,
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
        const auto& description = target.description();
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.image = target.native();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, description.mip_levels, 0, description.array_layers};
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
                    original.image = clone->native();
                    original.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    original.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    original.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    original.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                    original.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    original.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    dependency.pImageMemoryBarriers = &original;
                    vkCmdPipelineBarrier2(command, &dependency);
                    for (std::uint32_t mip = 0; mip < description.mip_levels; ++mip)
                    {
                        VkImageCopy copy{};
                        copy.srcSubresource =
                            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, description.array_layers};
                        copy.extent = {
                            std::max(1u, description.extent.width >> mip),
                            std::max(1u, description.extent.height >> mip),
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
                    original.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
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
                page.buffer().native(),
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
                !distinct || acquiring ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE;
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
            session.device().native(),
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
            double((values[1] - values[0]) & mask) * session.device().properties().limits.timestampPeriod
        );
        ++stats.gpu_samples;
        return {};
    }

    UploadContext::Task UploadContext::writeBuffer(std::shared_ptr<Buffer> target, RenderPayload source) noexcept
    {
        return write(std::move(target), {}, std::move(source), {}, {}, nullptr);
    }

    UploadContext::Task UploadContext::writeImage(
        std::shared_ptr<Image> target,
        RenderPayload source,
        std::vector<ImageTransferRegion> regions,
        std::shared_ptr<const Image> clone,
        SubmissionRequirements* clone_use
    ) noexcept
    {
        return write({}, std::move(target), std::move(source), std::move(regions), std::move(clone), clone_use);
    }

    UploadContext::Task UploadContext::write(
        std::shared_ptr<Buffer> buffer,
        std::shared_ptr<Image> image,
        RenderPayload payload,
        std::vector<ImageTransferRegion> regions,
        std::shared_ptr<const Image> clone,
        SubmissionRequirements* clone_use
    ) noexcept
    {
        const auto source = payload.bytes().view();
        if (source.empty())
        {
            co_return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (buffer)
        {
            const bool invalid_buffer = source.size() % 4 || source.size() > buffer->size() ||
                                        !(buffer->usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            if (invalid_buffer)
            {
                co_return cxx::unexpected(RenderError{kInvalidArgument});
            }
        }
        else
        {
            if (!image || regions.empty() || (bool(clone) != bool(clone_use)))
            {
                co_return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto& description = image->description();
            const bool invalid_image =
                description.samples != VK_SAMPLE_COUNT_1_BIT || !(description.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (invalid_image)
            {
                co_return cxx::unexpected(RenderError{kInvalidArgument});
            }
            for (const auto& region : regions)
            {
                const bool invalid_shape =
                    !region.block_extent || !region.block_bytes || !region.extent.width || !region.extent.height ||
                    region.extent.depth != 1 || region.offset.x < 0 || region.offset.y < 0 || region.offset.z != 0 ||
                    region.subresource.mipLevel >= description.mip_levels ||
                    region.subresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || region.subresource.layerCount != 1 ||
                    region.subresource.baseArrayLayer >= description.array_layers;
                if (invalid_shape)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const auto width = std::max(1u, description.extent.width >> region.subresource.mipLevel);
                const auto height = std::max(1u, description.extent.height >> region.subresource.mipLevel);
                const bool invalid_offset =
                    std::uint32_t(region.offset.x) > width || std::uint32_t(region.offset.y) > height;
                if (invalid_offset)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const bool invalid_extent =
                    region.extent.width > width - region.offset.x || region.extent.height > height - region.offset.y;
                const bool invalid_blocks =
                    std::uint32_t(region.offset.x) % region.block_extent ||
                    std::uint32_t(region.offset.y) % region.block_extent ||
                    (region.extent.width % region.block_extent && region.extent.width != width - region.offset.x) ||
                    (region.extent.height % region.block_extent && region.extent.height != height - region.offset.y);
                const auto row_bytes =
                    ((std::uint64_t(region.extent.width) + region.block_extent - 1) / region.block_extent) *
                    region.block_bytes;
                const auto row_count =
                    (std::uint64_t(region.extent.height) + region.block_extent - 1) / region.block_extent;
                const bool invalid_pitch = region.source_row_pitch < row_bytes || row_bytes > options.chunk_bytes;
                if (invalid_extent || invalid_blocks || invalid_pitch)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const auto stride = cxx::checkedMul<std::uint64_t>(row_count - 1, region.source_row_pitch);
                if (!stride)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const auto bytes = cxx::checkedAdd(*stride, row_bytes);
                const bool invalid_source =
                    !bytes || region.source_offset > source.size() || *bytes > source.size() - region.source_offset;
                if (invalid_source)
                {
                    co_return cxx::unexpected(RenderError{kInvalidArgument});
                }
            }
        }
        const auto retained = payload.retainedBytes();
        if (retained > options.pending_bytes - stats.pending_bytes)
        {
            co_return cxx::unexpected(RenderError{kCapacity});
        }
        auto acquired = session.staging_arena_->acquire();
        if (!acquired)
        {
            co_return cxx::unexpected(acquired.error());
        }
        auto page = std::move(*acquired);
        auto retirement = session.retirement().reserve();
        std::optional<SubmissionTicket> ticket;
        cxx::scope_exit release_page(
            [&]() noexcept
            {
                if (ticket)
                {
                    SubmissionRequirements use;
                    use.merge(*ticket);
                    if (!session.retirement().retire(retirement, use, page))
                    {
                        std::terminate(); // This Owner acquired both the reservation and the actual Copy ticket.
                    }
                }
            }
        );
        stats.pending_bytes += retained;
        stats.pending_peak = std::max(stats.pending_peak, stats.pending_bytes);
        cxx::scope_exit budget(
            [&]() noexcept
            {
                payload = {};
                stats.pending_bytes -= retained;
            }
        );
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        auto& graphics = session.queue();
        auto& copy = clone ? graphics : *copy_queue;
        const bool distinct = &copy != &graphics;
        const auto timestamp_bits = &copy == &graphics ? graphics_timestamp_bits : copy_timestamp_bits;
        const bool timestamp = timestamps && timestamp_bits;
        // A stop or device loss after accepted work cannot claim that the command had no side effects.
        const auto failure = [&](RenderError error) noexcept
        {
            const bool is_device_lost = error.type == kNativeFailure &&
                error.args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST);
            const bool is_unknown = ticket && (stop.stop_requested() || is_device_lost);
            return cxx::unexpected(is_unknown ? RenderError{kOutcomeUnknown} : error);
        };
        SubmissionRequirements accepted;
        std::size_t offset = 0;
        std::size_t region_index = 0;
        std::uint32_t row = 0;
        for (;;)
        {
            if (stop.stop_requested())
            {
                co_return failure(RenderError{kWorkStopped});
            }
            const auto* region = image ? &regions[region_index] : nullptr;
            const auto row_bytes =
                region ? std::size_t((region->extent.width - 1) / region->block_extent + 1) * region->block_bytes : 0;
            const auto rows = region ? std::uint32_t(std::min<std::size_t>(
                                           (region->extent.height - 1) / region->block_extent + 1 - row,
                                           options.chunk_bytes / row_bytes
                                       ))
                                     : 0;
            const auto chunk_size =
                region ? rows * row_bytes : std::min(source.size() - offset, options.chunk_bytes & ~std::size_t{3});
            const bool final = region ? region_index + 1 == regions.size() &&
                                            row + rows == (region->extent.height - 1) / region->block_extent + 1
                                      : offset + chunk_size == source.size();
            auto prepared = co_await (
                stdexec::schedule(cpu) |
                stdexec::then(
                    [&]() noexcept -> RenderResult<std::uint64_t>
                    {
                        LUX_RECORD("cpuPrepare", (void)0);
                        const auto started = UploadClock::now();
                        auto* destination = page->buffer().mappedBytes().data();
                        const auto* input =
                            source.data() + (region ? region->source_offset + row * region->source_row_pitch : offset);
                        if (!region || region->source_row_pitch == row_bytes)
                        {
                            std::memcpy(destination, input, chunk_size);
                        }
                        else
                        {
                            for (std::uint32_t r = 0; r < rows; ++r)
                            {
                                std::memcpy(
                                    destination + r * row_bytes,
                                    input + r * region->source_row_pitch,
                                    row_bytes
                                );
                            }
                        }
                        return elapsed(started);
                    }
                ) |
                stdexec::upon_error(
                    [](execution::EExecutionError error) noexcept -> RenderResult<std::uint64_t>
                    { return cxx::unexpected(RenderError{kCpuScheduleFailure, {std::uint64_t(error)}}); }
                ) |
                stdexec::upon_stopped(
                    []() noexcept -> RenderResult<std::uint64_t> { return cxx::unexpected(RenderError{kCpuStopped}); }
                )
            );
            if (!prepared)
            {
                co_return failure(prepared.error());
            }
            stats.prepare_ns += *prepared;
            stats.copied_bytes += chunk_size;
            if (stop.stop_requested())
            {
                co_return failure(RenderError{kWorkStopped});
            }
            std::optional<CommandBatch> batch;
            for (;;)
            {
                auto available = copy.begin();
                if (available)
                {
                    batch.emplace(std::move(*available));
                    break;
                }
                if (available.error().type != kCapacity)
                {
                    co_return failure(available.error());
                }
                const auto pending = copy.oldestPending();
                if (!pending)
                {
                    co_return failure(available.error());
                }
                auto completed = co_await copy.whenComplete(*pending);
                if (!completed)
                {
                    co_return failure(completed.error());
                }
                if (stop.stop_requested())
                {
                    co_return failure(RenderError{kWorkStopped});
                }
            }
            const auto recording_started = UploadClock::now();
            const auto flushed = page->buffer().flush(0, chunk_size);
            if (!flushed)
            {
                co_return failure(flushed.error());
            }
            if (buffer)
            {
                batch->retain(buffer);
            }
            else
            {
                batch->retain(image);
            }
            if (!ticket && clone)
            {
                batch->retain(clone);
            }
            if (timestamp)
            {
                vkResetQueryPool(session.device().native(), timestamps, std::uint32_t(page->index() * 2), 2);
                vkCmdWriteTimestamp2(
                    batch->native(),
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    timestamps,
                    std::uint32_t(page->index() * 2)
                );
            }
            if (buffer)
            {
                recordBuffer(batch->native(), *buffer, *page, offset, chunk_size, copy, graphics, false, final);
            }
            else
            {
                recordImage(
                    batch->native(),
                    *image,
                    *page,
                    *region,
                    clone.get(),
                    row,
                    rows,
                    !ticket,
                    copy,
                    graphics,
                    false,
                    final
                );
            }
            if (timestamp)
            {
                vkCmdWriteTimestamp2(
                    batch->native(),
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    timestamps,
                    std::uint32_t(page->index() * 2 + 1)
                );
            }
            auto submitted = std::move(*batch).submit();
            stats.owner_ns += elapsed(recording_started);
            if (!submitted)
            {
                co_return failure(submitted.error());
            }
            accepted.merge(*submitted);
            if (!ticket && clone_use)
            {
                clone_use->merge(*submitted);
            }
            ticket = *submitted;
            if (timestamp)
            {
                page->markMeasurement(*this, timestamp_bits);
            }
            ++stats.chunks;
            if (final)
            {
                break;
            }
            // Reuse only after actual Copy completion. On exit the page moves to the unique retirement facility.
            auto completed = co_await copy.whenComplete(*ticket);
            if (!completed)
            {
                co_return failure(completed.error());
            }
            if (auto measured = page->finishMeasurement(); !measured)
            {
                co_return failure(measured.error());
            }
            offset += chunk_size;
            if (region)
            {
                row += rows;
                if (row == (region->extent.height - 1) / region->block_extent + 1)
                {
                    ++region_index;
                    row = 0;
                }
            }
        }
        if (distinct)
        {
            std::optional<CommandBatch> batch;
            for (;;)
            {
                auto available = graphics.begin();
                if (available)
                {
                    batch.emplace(std::move(*available));
                    break;
                }
                if (available.error().type != kCapacity)
                {
                    co_return failure(available.error());
                }
                const auto pending = graphics.oldestPending();
                if (!pending)
                {
                    co_return failure(available.error());
                }
                auto completed = co_await graphics.whenComplete(*pending);
                if (!completed)
                {
                    co_return failure(completed.error());
                }
                if (stop.stop_requested())
                {
                    co_return failure(RenderError{kWorkStopped});
                }
            }
            const auto recording_started = UploadClock::now();
            if (buffer)
            {
                batch->retain(buffer);
                recordBuffer(batch->native(), *buffer, *page, 0, 0, copy, graphics, true, true);
            }
            else
            {
                batch->retain(image);
                recordImage(
                    batch->native(),
                    *image,
                    *page,
                    regions.back(),
                    nullptr,
                    0,
                    0,
                    false,
                    copy,
                    graphics,
                    true,
                    true
                );
            }
            const SubmissionWait wait{*ticket, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
            auto submitted = std::move(*batch).submit(std::span(&wait, 1));
            stats.owner_ns += elapsed(recording_started);
            if (!submitted)
            {
                co_return failure(submitted.error());
            }
            accepted.merge(*submitted);
        }
        co_return accepted;
    }
} // namespace lux::render::vulkan
