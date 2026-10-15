#include "UploadState.hpp"
#include "Native.hpp"
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <vk_mem_alloc.h>

namespace lux::render::vulkan
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        std::uint64_t nanoseconds(Clock::time_point start) noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        }
    }

    DeviceSession::UploadState::~UploadState() noexcept
    {
        // Construction failure or Session's joined + drained teardown; no caller may retain a candidate here.
        for (auto& page : pages)
        {
            if (page.leased) std::terminate();
            if (page.buffer) session.destroyFinished(page.buffer);
        }
        if (timestamps) vkDestroyQueryPool(session.device().native(), timestamps, nullptr);
    }

    RenderResult<void> DeviceSession::configureUploads(const TransferOptions& options) noexcept
    {
        const bool invalid = uploads_ || !options.enqueue || !options.join || options.chunk_bytes < 256 ||
            options.chunk_bytes > 16 * 1024 * 1024 || options.staging_pages == 0 || options.staging_pages > 16 ||
            options.pending_bytes < options.chunk_bytes;
        if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
        if (options.measure_gpu && !device_.hostQueryReset()) return cxx::unexpected(RenderError{kUnsupported});
        auto state = std::make_unique<UploadState>(*this, options);
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
            if (result != VK_SUCCESS) return cxx::unexpected(nativeError(result));
        }
        state->pages.resize(options.staging_pages);
        const std::array families{queue().nativeQueue().family, state->copy_queue->nativeQueue().family};
        const auto sharing = families[0] == families[1] ? std::span<const std::uint32_t>{} : std::span(families);
        for (auto& page : state->pages)
        {
            auto buffer = createBufferRecord(options.chunk_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                EMemoryAccess::UPLOAD, sharing);
            if (!buffer) return cxx::unexpected(buffer.error());
            page.buffer = *buffer;
            page.permit = BufferPermit{*this, (*buffer)->key_};
            page.mapped = static_cast<std::byte*>((*buffer)->mapped_);
        }
        state->stats.staging_bytes = options.chunk_bytes * options.staging_pages;
        uploads_ = std::move(state);
        return {};
    }

    void DeviceSession::joinUploads() noexcept
    {
        if (uploads_) uploads_->options.join(uploads_->options.user);
    }

    TransferStats DeviceSession::uploadStats() const noexcept
    {
        return uploads_ ? uploads_->stats : TransferStats{};
    }

    RenderResult<std::unique_ptr<Transfer>> DeviceSession::uploadImage(
        const ImageDescription& description, std::span<const std::byte> source,
        std::span<const ImageTransferRegion> regions, const ImageAllocation* clone, bool graphics_queue
    ) noexcept
    {
        if (!uploads_) return cxx::unexpected(RenderError{kUnsupported});
        const bool invalid_shape = !description.extent.width || !description.extent.height ||
            description.samples != VK_SAMPLE_COUNT_1_BIT || !description.array_layers ||
            !description.mip_levels || description.mip_levels >
                std::bit_width(std::max(description.extent.width, description.extent.height));
        const bool invalid = invalid_shape || source.empty() || regions.empty() || regions.size() > 32 ||
            !(description.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || (clone && !graphics_queue);
        if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
        for (const auto& region : regions)
        {
            const bool invalid_shape = !region.block_extent || !region.block_bytes || !region.extent.width ||
                !region.extent.height || region.extent.depth != 1 || region.offset.x < 0 || region.offset.y < 0 ||
                region.offset.z != 0 || region.subresource.mipLevel >= description.mip_levels ||
                region.subresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                region.subresource.layerCount != 1 || region.subresource.baseArrayLayer >= description.array_layers;
            if (invalid_shape) return cxx::unexpected(RenderError{kInvalidArgument});
            const auto width = std::max(1u, description.extent.width >> region.subresource.mipLevel);
            const auto height = std::max(1u, description.extent.height >> region.subresource.mipLevel);
            const bool invalid_extent = std::uint32_t(region.offset.x) > width || std::uint32_t(region.offset.y) > height;
            if (invalid_extent) return cxx::unexpected(RenderError{kInvalidArgument});
            const bool invalid_range = region.extent.width > width - std::uint32_t(region.offset.x) ||
                region.extent.height > height - std::uint32_t(region.offset.y);
            const bool invalid_blocks = std::uint32_t(region.offset.x) % region.block_extent ||
                std::uint32_t(region.offset.y) % region.block_extent ||
                (region.extent.width % region.block_extent && region.extent.width != width - region.offset.x) ||
                (region.extent.height % region.block_extent && region.extent.height != height - region.offset.y);
            const auto row = ((std::uint64_t(region.extent.width) + region.block_extent - 1) / region.block_extent) *
                region.block_bytes;
            const auto rows = (std::uint64_t(region.extent.height) + region.block_extent - 1) / region.block_extent;
            const bool invalid_pitch = region.source_row_pitch < row || row > uploads_->options.chunk_bytes;
            if (invalid_range || invalid_blocks || invalid_pitch) return cxx::unexpected(RenderError{kInvalidArgument});
            auto size = cxx::checkedMul<std::uint64_t>(rows - 1, region.source_row_pitch);
            if (!size) return cxx::unexpected(RenderError{kInvalidArgument});
            auto end = cxx::checkedAdd(*size, row);
            if (!end || region.source_offset > source.size() || *end > source.size() - region.source_offset)
                return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto& state = *uploads_;
        auto page = std::find_if(state.pages.begin(), state.pages.end(), [](const auto& value) { return !value.leased; });
        if (page == state.pages.end() || source.size() > state.options.pending_bytes - state.stats.pending_bytes)
            return cxx::unexpected(RenderError{kCapacity});
        auto image = createImage(description);
        if (!image) return cxx::unexpected(image.error());
        auto result = std::unique_ptr<Transfer>(new Transfer(*this, source, std::size_t(page - state.pages.begin())));
        result->image_ = std::move(*image);
        result->regions_.assign(regions.begin(), regions.end());
        result->clone_ = clone;
        result->copy_queue_ = graphics_queue ? &queue() : state.copy_queue;
        return result;
    }

    RenderResult<std::unique_ptr<Transfer>> DeviceSession::uploadBuffer(
        std::span<const std::byte> source, VkBufferUsageFlags usage
    ) noexcept
    {
        if (!uploads_) return cxx::unexpected(RenderError{kUnsupported});
        if (source.empty() || source.size() % 4) return cxx::unexpected(RenderError{kInvalidArgument});
        auto& state = *uploads_;
        auto page = std::find_if(state.pages.begin(), state.pages.end(), [](const auto& value) { return !value.leased; });
        if (page == state.pages.end() || source.size() > state.options.pending_bytes - state.stats.pending_bytes)
            return cxx::unexpected(RenderError{kCapacity});
        auto buffer = createBuffer(source.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE);
        if (!buffer) return cxx::unexpected(buffer.error());
        auto result = std::unique_ptr<Transfer>(new Transfer(*this, source, std::size_t(page - state.pages.begin())));
        result->buffer_ = std::move(*buffer);
        result->copy_queue_ = state.copy_queue;
        return result;
    }

    Transfer::Transfer(DeviceSession& session, std::span<const std::byte> source, std::size_t page) noexcept
        : session_(&session), source_(source), mapped_(session.uploads_->pages[page].mapped), page_(page)
    {
        auto& state = *session.uploads_;
        state.pages[page].leased = true;
        state.stats.pending_bytes += source.size();
        state.stats.pending_peak = std::max(state.stats.pending_peak, state.stats.pending_bytes);
    }

    Transfer::~Transfer() noexcept
    {
        const bool cpu_active = phase_ == EPhase::CPU_PENDING && !cpu_ready_.load(std::memory_order_acquire);
        if (cpu_active) std::terminate();
        if (image_)
        {
            auto result = ticket_ ? session_->releaseCompleted(image_, *ticket_) : session_->destroyUnsubmitted(image_);
            if (!result) std::terminate();
        }
        if (buffer_)
        {
            auto result = ticket_ ? session_->releaseCompleted(buffer_, *ticket_) : session_->destroyUnsubmitted(buffer_);
            if (!result) std::terminate();
        }
        session_->uploads_->pages[page_].leased = false;
        session_->uploads_->stats.pending_bytes -= source_.size();
    }

    VkDevice Transfer::device() const noexcept { return session_->device().native(); }

    const ImageAllocation& Transfer::image() const noexcept { return *session_->borrow(image_); }

    const BufferAllocation& Transfer::buffer() const noexcept { return *session_->borrow(buffer_); }

    RenderResult<ImagePermit> Transfer::takeImage() noexcept
    {
        if (!completed() || !image_) return cxx::unexpected(RenderError{kBusy});
        return std::move(image_);
    }

    RenderResult<BufferPermit> Transfer::takeBuffer() noexcept
    {
        if (!completed() || !buffer_) return cxx::unexpected(RenderError{kBusy});
        return std::move(buffer_);
    }

    void Transfer::prepare(void* pointer) noexcept
    {
        auto& transfer = *static_cast<Transfer*>(pointer);
        const auto start = Clock::now();
        if (transfer.regions_.empty())
            std::memcpy(transfer.mapped_, transfer.source_.data() + transfer.offset_, transfer.chunk_size_);
        else
        {
            const auto& region = transfer.regions_[transfer.region_];
            const auto* source = transfer.source_.data() + region.source_offset + transfer.row_ * region.source_row_pitch;
            if (region.source_row_pitch == transfer.row_bytes_)
                std::memcpy(transfer.mapped_, source, transfer.chunk_size_);
            else
                for (std::uint32_t row = 0; row < transfer.rows_; ++row)
                    std::memcpy(transfer.mapped_ + row * transfer.row_bytes_,
                        source + row * region.source_row_pitch, transfer.row_bytes_);
        }
        transfer.prepare_ns_ = nanoseconds(start);
        transfer.cpu_ready_.store(true, std::memory_order_release);
    }

    RenderResult<bool> Transfer::advance() noexcept
    {
        const auto start = Clock::now();
        auto& state = *session_->uploads_;
        struct Cost final
        {
            TransferStats& stats;
            Clock::time_point start;
            ~Cost() { stats.owner_ns += nanoseconds(start); }
        } cost{state.stats, start};
        if (phase_ == EPhase::COMPLETE) return true;
        auto& graphics = session_->queue();
        const bool distinct = copy_queue_ != &graphics;
        if (phase_ == EPhase::CPU_PENDING)
        {
            if (!cpu_ready_.load(std::memory_order_acquire)) return false;
            state.stats.prepare_ns += prepare_ns_;
            state.stats.copied_bytes += chunk_size_;
            phase_ = EPhase::COPY;
        }
        if (ticket_)
        {
            auto* owner = session_->queueFor(*ticket_);
            auto completed = owner->poll();
            if (!completed)
            {
                if (cancelled_ && owner->deviceLost()) { phase_ = EPhase::COMPLETE; return true; }
                return cxx::unexpected(completed.error());
            }
            if (*completed < ticket_->serial()) return false;
        }
        const auto timestamp_bits = copy_queue_ == &graphics ? state.graphics_timestamp_bits : state.copy_timestamp_bits;
        if (query_pending_)
        {
            std::array<std::uint64_t, 2> stamps{};
            const auto result = vkGetQueryPoolResults(device(), state.timestamps, std::uint32_t(page_ * 2), 2,
                sizeof(stamps), stamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result != VK_SUCCESS) return cxx::unexpected(nativeError(result));
            const auto mask = timestamp_bits == 64 ? UINT64_MAX : (std::uint64_t{1} << timestamp_bits) - 1;
            state.stats.gpu_ns += std::uint64_t(double((stamps[1] - stamps[0]) & mask) *
                session_->device().properties().limits.timestampPeriod);
            ++state.stats.gpu_samples;
            query_pending_ = false;
        }
        if (cancelled_) { phase_ = EPhase::COMPLETE; return true; }
        if (phase_ == EPhase::FINAL_PENDING) { phase_ = EPhase::COMPLETE; return true; }
        if (phase_ == EPhase::COPY_PENDING)
        {
            offset_ += chunk_size_;
            if (!regions_.empty())
            {
                row_ += rows_;
                const auto& region = regions_[region_];
                if (row_ == (region.extent.height - 1) / region.block_extent + 1)
                { ++region_; row_ = 0; }
            }
            const bool end = regions_.empty() ? offset_ == source_.size() : region_ == regions_.size();
            phase_ = end ? EPhase::ACQUIRE : EPhase::PREPARE;
        }
        if (phase_ == EPhase::PREPARE)
        {
            if (regions_.empty()) chunk_size_ = std::min(source_.size() - offset_, state.options.chunk_bytes & ~std::size_t{3});
            else
            {
                const auto& region = regions_[region_];
                row_bytes_ = std::size_t((region.extent.width - 1) / region.block_extent + 1) * region.block_bytes;
                rows_ = std::min<std::size_t>((region.extent.height - 1) / region.block_extent + 1 - row_,
                    state.options.chunk_bytes / row_bytes_);
                chunk_size_ = rows_ * row_bytes_;
            }
            cpu_ready_.store(false, std::memory_order_relaxed);
            if (!state.options.enqueue(state.options.user, prepare, this)) return cxx::unexpected(RenderError{kCapacity});
            phase_ = EPhase::CPU_PENDING;
            return false;
        }
        const bool acquiring = phase_ == EPhase::ACQUIRE;
        auto& queue = acquiring ? graphics : *copy_queue_;
        auto batch = queue.begin();
        if (!batch)
        {
            if (batch.error().type == kCapacity || batch.error().type == kBusy) return false;
            return cxx::unexpected(batch.error());
        }
        const bool timestamp = !acquiring && state.timestamps && timestamp_bits;
        if (timestamp)
        {
            vkResetQueryPool(device(), state.timestamps, std::uint32_t(page_ * 2), 2);
            vkCmdWriteTimestamp2(batch->native(), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, state.timestamps,
                std::uint32_t(page_ * 2));
        }
        if (!acquiring)
        {
            const auto& page = state.pages[page_];
            const auto flushed = LUX_NATIVE("flush", vmaFlushAllocation(session_->allocator_.native(),
                page.buffer->allocation_, 0, chunk_size_));
            if (flushed != VK_SUCCESS) return cxx::unexpected(nativeError(flushed));
        }
        bool final = acquiring;
        if (!acquiring)
        {
            if (regions_.empty()) final = offset_ + chunk_size_ == source_.size();
            else
            {
                const auto& region = regions_[region_];
                final = region_ + 1 == regions_.size() &&
                    row_ + rows_ == (region.extent.height - 1) / region.block_extent + 1;
            }
        }
        if (regions_.empty()) recordBuffer(batch->native(), acquiring, final);
        else recordImage(batch->native(), acquiring, final);
        const std::optional<SubmissionWait> wait = acquiring ?
            std::optional{SubmissionWait{*ticket_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}} : std::nullopt;
        if (timestamp) vkCmdWriteTimestamp2(batch->native(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            state.timestamps, std::uint32_t(page_ * 2 + 1));
        auto accepted = std::move(*batch).submit(wait ? std::span(&*wait, 1) : std::span<const SubmissionWait>{});
        if (!accepted) return cxx::unexpected(accepted.error());
        ticket_ = *accepted;
        query_pending_ = timestamp;
        initialized_ = true;
        if (!acquiring) ++state.stats.chunks;
        phase_ = final && (!distinct || acquiring) ? EPhase::FINAL_PENDING : EPhase::COPY_PENDING;
        return false;
    }

    void Transfer::recordBuffer(VkCommandBuffer command, bool acquiring, bool final) noexcept
    {
        auto& graphics = session_->queue();
        const bool distinct = copy_queue_ != &graphics;
        const bool cross_family = copy_queue_->nativeQueue().family != graphics.nativeQueue().family;
        if (!acquiring)
        {
            const VkBufferCopy copy{0, offset_, chunk_size_};
            vkCmdCopyBuffer(command, session_->uploads_->pages[page_].buffer->native(), buffer().native(), 1, &copy);
        }
        if (!final) return;
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.buffer = buffer().native();
        barrier.size = VK_WHOLE_SIZE;
        barrier.srcStageMask = acquiring ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = acquiring ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = !distinct || acquiring ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = !distinct || acquiring ? VK_ACCESS_2_MEMORY_READ_BIT : VK_ACCESS_2_NONE;
        barrier.srcQueueFamilyIndex = cross_family ? copy_queue_->nativeQueue().family : VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = cross_family ? graphics.nativeQueue().family : VK_QUEUE_FAMILY_IGNORED;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    void Transfer::recordImage(VkCommandBuffer command, bool acquiring, bool final) noexcept
    {
        auto& graphics = session_->queue();
        const bool distinct = copy_queue_ != &graphics;
        const bool cross_family = copy_queue_->nativeQueue().family != graphics.nativeQueue().family;
        const auto& description = image().description();
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.image = image().native();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, description.mip_levels, 0, description.array_layers};
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        if (!acquiring)
        {
            if (!initialized_)
            {
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                vkCmdPipelineBarrier2(command, &dependency);
                if (clone_)
                {
                    auto original = barrier;
                    original.image = clone_->native();
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
                        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, description.array_layers};
                        copy.extent = {std::max(1u, description.extent.width >> mip), std::max(1u, description.extent.height >> mip), 1};
                        vkCmdCopyImage(command, original.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
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
            const auto& input = regions_[region_];
            VkBufferImageCopy region{};
            region.imageSubresource = input.subresource;
            region.imageOffset = input.offset;
            region.imageOffset.y += std::int32_t(row_ * input.block_extent);
            region.imageExtent = {input.extent.width,
                std::uint32_t(std::min(std::uint64_t(rows_) * input.block_extent,
                    std::uint64_t(input.extent.height - row_ * input.block_extent))), 1};
            vkCmdCopyBufferToImage(command, session_->uploads_->pages[page_].buffer->native(), barrier.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        if (final)
        {
            barrier.srcStageMask = acquiring ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = acquiring ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = !distinct || acquiring ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = !distinct || acquiring ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE;
            barrier.oldLayout = acquiring && !cross_family ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (cross_family)
            {
                barrier.srcQueueFamilyIndex = copy_queue_->nativeQueue().family;
                barrier.dstQueueFamilyIndex = graphics.nativeQueue().family;
            }
            vkCmdPipelineBarrier2(command, &dependency);
        }
    }
}

