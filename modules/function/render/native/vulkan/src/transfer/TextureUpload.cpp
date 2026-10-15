#include "UploadState.hpp"
#include "Native.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
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

        struct Format final { VkFormat native; std::uint32_t extent, bytes; bool srgb; };
        Format format(rdesc::ETexturePixelFormat value) noexcept
        {
            switch (value)
            {
            case rdesc::ETexturePixelFormat::RGBA8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, false};
            case rdesc::ETexturePixelFormat::RGBA8_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, 1, 4, true};
            case rdesc::ETexturePixelFormat::BC1_SRGB: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 4, 8, true};
            default: return {};
            }
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

    TextureUploadStats DeviceSession::uploadStats() const noexcept
    {
        return uploads_ ? uploads_->stats : TextureUploadStats{};
    }

    RenderResult<std::unique_ptr<TextureUpload>> DeviceSession::uploadTexture(
        const rdesc::TextureInfo& info, std::span<const std::byte> source) noexcept
    {
        return prepareTextureUpload(info, source, {});
    }

    RenderResult<std::unique_ptr<TextureUpload>> DeviceSession::prepareTextureUpload(
        const rdesc::TextureInfo& info, std::span<const std::byte> source, std::optional<TextureRegion> region) noexcept
    {
        const auto layout = format(info.pixel_format);
        const auto max_dimension = device_.properties().limits.maxImageDimension2D;
        const bool bad_extent = info.width <= 0 || info.height <= 0 ||
            std::uint32_t(info.width) > max_dimension || std::uint32_t(info.height) > max_dimension;
        const bool bad_count = info.layers != 1 || info.mip_count == 0 || info.mip_count > rdesc::kTextureMaxMipCount;
        const bool bad_color = info.color_space == rdesc::ETextureColorSpace::UNKNOWN ||
            info.color_space > rdesc::ETextureColorSpace::DATA ||
            (layout.srgb != (info.color_space == rdesc::ETextureColorSpace::SRGB));
        const bool premultiplied_srgb = layout.srgb &&
            rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::PREMULTIPLIED_ALPHA);
        const bool bad_flags = (info.flags & ~7u) != 0 ||
            (rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::COMPRESSED) != (layout.extent == 4)) ||
            (rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::NO_MIPS) && info.mip_count != 1);
        if (!uploads_ || layout.native == VK_FORMAT_UNDEFINED) return cxx::unexpected(RenderError{kUnsupported});
        if (bad_extent || bad_count || bad_color || bad_flags || info.channel != 4)
            return cxx::unexpected(RenderError{kInvalidArgument});
        if (layout.extent == 4 && !device_.textureCompressionBc()) return cxx::unexpected(RenderError{kUnsupported});
        if (premultiplied_srgb) return cxx::unexpected(RenderError{kUnsupported});
        const auto max_mips = std::bit_width(std::max(std::uint32_t(info.width), std::uint32_t(info.height)));
        if (info.mip_count > max_mips) return cxx::unexpected(RenderError{kInvalidArgument});
        std::uint64_t expected_offset = 0;
        for (std::uint32_t mip = 0; mip < info.mip_count; ++mip)
        {
            const auto width = std::max(1u, std::uint32_t(info.width) >> mip);
            const auto height = std::max(1u, std::uint32_t(info.height) >> mip);
            const auto rows = (height + layout.extent - 1) / layout.extent;
            const std::uint64_t row = std::uint64_t((width + layout.extent - 1) / layout.extent) * layout.bytes;
            const auto size = cxx::checkedMul<std::uint64_t>(row, std::uint64_t(rows));
            if (!size) return cxx::unexpected(RenderError{kInvalidArgument});
            const auto& input = info.mip_ranges[mip];
            const bool mismatch = input.width != width || input.height != height || input.size != *size ||
                input.offset != expected_offset || row > uploads_->options.chunk_bytes;
            if (mismatch) return cxx::unexpected(RenderError{kInvalidArgument});
            auto total = cxx::checkedAdd(expected_offset, *size);
            if (!total) return cxx::unexpected(RenderError{kInvalidArgument});
            expected_offset = *total;
        }
        if (region)
        {
            if (region->mip >= info.mip_count) return cxx::unexpected(RenderError{kInvalidArgument});
            const auto& mip = info.mip_ranges[region->mip];
            const bool invalid_offset = region->x >= mip.width || region->y >= mip.height;
            if (invalid_offset) return cxx::unexpected(RenderError{kInvalidArgument});
            const bool invalid_range = !region->width || !region->height || region->width > mip.width - region->x ||
                region->height > mip.height - region->y;
            const bool invalid_blocks = region->x % layout.extent || region->y % layout.extent ||
                (region->width % layout.extent && region->width != mip.width - region->x) ||
                (region->height % layout.extent && region->height != mip.height - region->y);
            if (invalid_range || invalid_blocks) return cxx::unexpected(RenderError{kInvalidArgument});
            const auto rows = (region->height + layout.extent - 1) / layout.extent;
            const std::uint64_t row = ((region->width + layout.extent - 1) / layout.extent) * layout.bytes;
            const bool invalid_pitch = region->row_pitch < row || region->row_pitch % layout.bytes ||
                row > uploads_->options.chunk_bytes;
            if (invalid_pitch) return cxx::unexpected(RenderError{kInvalidArgument});
            auto stride = cxx::checkedMul<std::uint64_t>(rows - 1, region->row_pitch);
            if (!stride) return cxx::unexpected(RenderError{kInvalidArgument});
            auto total = cxx::checkedAdd(*stride, row);
            if (!total) return cxx::unexpected(RenderError{kInvalidArgument});
            expected_offset = *total;
        }
        if (expected_offset != source.size() || source.empty()) return cxx::unexpected(RenderError{kInvalidArgument});
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device_.physical(), layout.native, &properties);
        constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((properties.optimalTilingFeatures & required) != required)
            return cxx::unexpected(RenderError{kUnsupported});
        auto& state = *uploads_;
        const auto page = std::find_if(state.pages.begin(), state.pages.end(), [](const auto& p) { return !p.leased; });
        const bool full = page == state.pages.end() || source.size() > state.options.pending_bytes - state.stats.pending_bytes;
        if (full) return cxx::unexpected(RenderError{kCapacity});
        auto image = createImage({{std::uint32_t(info.width), std::uint32_t(info.height)}, layout.native,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_SAMPLE_COUNT_1_BIT, info.mip_count, 1});
        if (!image) return cxx::unexpected(image.error());
        auto candidate = std::unique_ptr<TextureUpload>(new TextureUpload(*this, info, source, std::move(*image),
            std::size_t(page - state.pages.begin()), layout.extent, layout.bytes));
        candidate->copy_queue_ = region ? &queue() : state.copy_queue;
        candidate->region_ = region;
        if (region) candidate->mip_ = region->mip;
        page->leased = true;
        state.stats.pending_bytes += source.size();
        state.stats.pending_peak = std::max(state.stats.pending_peak, state.stats.pending_bytes);
        return candidate;
    }

    TextureUpload::TextureUpload(DeviceSession& session, rdesc::TextureInfo info, std::span<const std::byte> source,
        ImagePermit image, std::size_t page, std::uint32_t extent, std::uint32_t bytes) noexcept
        : session_(&session), info_(info), source_(source), mapped_(session.uploads_->pages[page].mapped),
          image_(std::move(image)), page_(page), block_extent_(extent), block_bytes_(bytes) {}

    TextureUpload::~TextureUpload() noexcept
    {
        const bool cpu_active = phase_ == EPhase::CPU_PENDING && !cpu_ready_.load(std::memory_order_acquire);
        if (cpu_active) std::terminate();
        if (storage_) storage_->rollback(*this);
        if (base_) --base_->leases_;
        if (image_)
        {
            auto result = ticket_ ? session_->releaseCompleted(image_, *ticket_) : session_->destroyUnsubmitted(image_);
            if (!result) std::terminate();
        }
        session_->uploads_->pages[page_].leased = false;
        session_->uploads_->stats.pending_bytes -= source_.size();
        if (!storage_id_.isNull()) session_->resources().release(storage_id_);
    }

    VkDevice TextureUpload::device() const noexcept { return session_->device().native(); }

    const ImageAllocation& TextureUpload::image() const noexcept { return *session_->borrow(image_); }

    RenderResult<ImagePermit> TextureUpload::takeImage() noexcept
    {
        if (phase_ != EPhase::COMPLETE || cancelled_ || !image_ || storage_) return cxx::unexpected(RenderError{kBusy});
        return std::move(image_);
    }

    RenderResult<void> TextureUpload::publish() noexcept
    {
        if (phase_ != EPhase::COMPLETE || cancelled_ || !storage_ || !ticket_)
            return cxx::unexpected(RenderError{kBusy});
        storage_->publish(*this);
        return {};
    }

    void TextureUpload::prepare(void* pointer) noexcept
    {
        auto& upload = *static_cast<TextureUpload*>(pointer);
        const auto start = Clock::now();
        if (upload.region_)
        {
            for (std::uint32_t row = 0; row < upload.rows_; ++row)
                std::memcpy(upload.mapped_ + row * upload.row_bytes_,
                    upload.source_.data() + (upload.row_ + row) * upload.region_->row_pitch, upload.row_bytes_);
        }
        else
        {
            const auto offset = upload.info_.mip_ranges[upload.mip_].offset + upload.row_ * upload.row_bytes_;
            std::memcpy(upload.mapped_, upload.source_.data() + offset, upload.chunk_size_);
        }
        upload.prepare_ns_ = nanoseconds(start);
        upload.cpu_ready_.store(true, std::memory_order_release);
    }

    RenderResult<bool> TextureUpload::advance() noexcept
    {
        const auto start = Clock::now();
        auto& state = *session_->uploads_;
        struct Cost final
        {
            TextureUploadStats& stats;
            Clock::time_point start;
            ~Cost() { stats.owner_ns += nanoseconds(start); }
        } cost{state.stats, start};
        if (phase_ == EPhase::COMPLETE) return true;
        auto& transfer = *copy_queue_;
        auto& graphics = session_->queue();
        const bool distinct = &transfer != &graphics;
        const bool cross_family = transfer.nativeQueue().family != graphics.nativeQueue().family;
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
        if (query_pending_)
        {
            std::array<std::uint64_t, 2> stamps{};
            const auto result = vkGetQueryPoolResults(device(), state.timestamps, std::uint32_t(page_ * 2), 2,
                sizeof(stamps), stamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result != VK_SUCCESS) return cxx::unexpected(nativeError(result));
            const auto bits = region_ ? state.graphics_timestamp_bits : state.copy_timestamp_bits;
            const auto mask = bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1;
            state.stats.gpu_ns += std::uint64_t(double((stamps[1] - stamps[0]) & mask) *
                session_->device().properties().limits.timestampPeriod);
            ++state.stats.gpu_samples;
            query_pending_ = false;
        }
        if (cancelled_) { phase_ = EPhase::COMPLETE; return true; }
        if (phase_ == EPhase::FINAL_PENDING) { phase_ = EPhase::COMPLETE; return true; }
        if (phase_ == EPhase::COPY_PENDING)
        {
            row_ += rows_;
            const auto height = region_ ? region_->height : info_.mip_ranges[mip_].height;
            if (row_ == (height + block_extent_ - 1) / block_extent_) { ++mip_; row_ = 0; }
            phase_ = mip_ == info_.mip_count ? EPhase::ACQUIRE : EPhase::PREPARE;
        }
        if (phase_ == EPhase::PREPARE)
        {
            const auto& mip = info_.mip_ranges[mip_];
            const auto width = region_ ? region_->width : mip.width;
            const auto height = region_ ? region_->height : mip.height;
            row_bytes_ = ((width + block_extent_ - 1) / block_extent_) * block_bytes_;
            rows_ = std::min<std::size_t>((height + block_extent_ - 1) / block_extent_ - row_,
                state.options.chunk_bytes / row_bytes_);
            chunk_size_ = rows_ * row_bytes_;
            cpu_ready_.store(false, std::memory_order_relaxed);
            if (!state.options.enqueue(state.options.user, prepare, this)) return cxx::unexpected(RenderError{kCapacity});
            phase_ = EPhase::CPU_PENDING;
            return false;
        }
        auto& queue = phase_ == EPhase::ACQUIRE ? graphics : transfer;
        auto batch = queue.begin();
        if (!batch)
        {
            if (batch.error().type == kCapacity || batch.error().type == kBusy) return false;
            return cxx::unexpected(batch.error());
        }
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.image = image().native();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, info_.mip_count, 0, 1};
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        const bool acquiring = phase_ == EPhase::ACQUIRE;
        const auto timestamp_bits = region_ ? state.graphics_timestamp_bits : state.copy_timestamp_bits;
        const bool timestamp = !acquiring && state.timestamps && timestamp_bits;
        if (timestamp)
        {
            // This page's preceding ticket has completed above. Host reset is valid on transfer-only families.
            vkResetQueryPool(device(), state.timestamps, std::uint32_t(page_ * 2), 2);
            vkCmdWriteTimestamp2(batch->native(), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, state.timestamps,
                std::uint32_t(page_ * 2));
        }
        bool final = acquiring;
        if (!acquiring)
        {
            auto& page = state.pages[page_];
            const auto flushed = LUX_NATIVE("flush", vmaFlushAllocation(session_->allocator_.native(),
                page.buffer->allocation_, 0, chunk_size_));
            if (flushed != VK_SUCCESS) return cxx::unexpected(nativeError(flushed));
            if (!initialized_)
            {
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                vkCmdPipelineBarrier2(batch->native(), &dependency);
                if (base_)
                {
                    auto original = barrier;
                    original.image = base_->image_->native();
                    original.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    original.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    original.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    original.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                    original.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    original.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    dependency.pImageMemoryBarriers = &original;
                    vkCmdPipelineBarrier2(batch->native(), &dependency);
                    for (std::uint32_t mip = 0; mip < info_.mip_count; ++mip)
                    {
                        VkImageCopy copy{};
                        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
                        copy.extent = {info_.mip_ranges[mip].width, info_.mip_ranges[mip].height, 1};
                        vkCmdCopyImage(batch->native(), original.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                    }
                    original.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    original.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                    original.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    original.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    original.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    original.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    vkCmdPipelineBarrier2(batch->native(), &dependency);
                    dependency.pImageMemoryBarriers = &barrier;
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    vkCmdPipelineBarrier2(batch->native(), &dependency); // clone -> overlapping patch WAW
                }
            }
            const auto& mip = info_.mip_ranges[mip_];
            const auto width = region_ ? region_->width : mip.width;
            const auto height = region_ ? region_->height : mip.height;
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip_, 0, 1};
            region.imageOffset = {region_ ? std::int32_t(region_->x) : 0,
                std::int32_t((region_ ? region_->y : 0) + row_ * block_extent_), 0};
            region.imageExtent = {width, std::min(rows_ * block_extent_, height - row_ * block_extent_), 1};
            vkCmdCopyBufferToImage(batch->native(), page.buffer->native(), barrier.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            final = (region_ || mip_ + 1 == info_.mip_count) &&
                row_ + rows_ == (height + block_extent_ - 1) / block_extent_;
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
                barrier.srcQueueFamilyIndex = transfer.nativeQueue().family;
                barrier.dstQueueFamilyIndex = graphics.nativeQueue().family;
            }
            vkCmdPipelineBarrier2(batch->native(), &dependency);
        }
        const std::optional<SubmissionWait> wait = acquiring ?
            std::optional{SubmissionWait{*ticket_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}} : std::nullopt;
        if (timestamp) vkCmdWriteTimestamp2(batch->native(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            state.timestamps, std::uint32_t(page_ * 2 + 1));
        auto accepted = std::move(*batch).submit(wait ? std::span(&*wait, 1) : std::span<const SubmissionWait>{});
        if (!accepted) return cxx::unexpected(accepted.error());
        ticket_ = *accepted;
        query_pending_ = timestamp;
        if (base_) base_->last_use_ = *accepted; // Graphics clone and any later patch dominate earlier readers
        initialized_ = true;
        if (!acquiring) ++state.stats.chunks;
        phase_ = final && (!distinct || acquiring) ? EPhase::FINAL_PENDING : EPhase::COPY_PENDING;
        return false;
    }
}
