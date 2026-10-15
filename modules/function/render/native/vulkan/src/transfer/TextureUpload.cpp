#include "UploadState.hpp"
#include "Native.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>
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
        const ImageDescription description{{std::uint32_t(info.width), std::uint32_t(info.height)}, layout.native,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_SAMPLE_COUNT_1_BIT, info.mip_count, 1};
        std::vector<ImageTransferRegion> regions;
        if (region)
            regions.push_back({0, region->row_pitch, layout.extent, layout.bytes,
                {VK_IMAGE_ASPECT_COLOR_BIT, region->mip, 0, 1},
                {std::int32_t(region->x), std::int32_t(region->y), 0}, {region->width, region->height, 1}});
        else
            for (std::uint32_t mip = 0; mip < info.mip_count; ++mip)
            {
                const auto& input = info.mip_ranges[mip];
                const auto pitch = std::uint64_t((input.width + layout.extent - 1) / layout.extent) * layout.bytes;
                regions.push_back({input.offset, pitch, layout.extent, layout.bytes,
                    {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1}, {}, {input.width, input.height, 1}});
            }
        auto transfer = uploadImage(description, source, regions, nullptr, region.has_value());
        if (!transfer) return cxx::unexpected(transfer.error());
        return std::unique_ptr<TextureUpload>(new TextureUpload(*this, info, std::move(*transfer)));
    }

    TextureUpload::TextureUpload(DeviceSession& session, rdesc::TextureInfo info, std::unique_ptr<Transfer> work) noexcept
        : session_(&session), info_(info), transfer_(std::move(work)) {}

    TextureUpload::~TextureUpload() noexcept
    {
        if (storage_) storage_->rollback(*this);
        if (base_) --base_->leases_;
        transfer_.reset();
        if (!storage_id_.isNull()) session_->resources().release(storage_id_);
    }

    VkDevice TextureUpload::device() const noexcept { return transfer_->device(); }

    const ImageAllocation& TextureUpload::image() const noexcept { return transfer_->image(); }

    RenderResult<ImagePermit> TextureUpload::takeImage() noexcept
    {
        if (storage_) return cxx::unexpected(RenderError{kBusy});
        return transfer_->takeImage();
    }

    RenderResult<void> TextureUpload::publish() noexcept
    {
        if (!transfer_->completed() || !storage_ || !transfer_->lastUse()) return cxx::unexpected(RenderError{kBusy});
        storage_->publish(*this);
        return {};
    }

    RenderResult<bool> TextureUpload::advance() noexcept
    {
        auto progress = transfer_->advance();
        if (base_ && transfer_->lastUse()) base_->last_use_ = transfer_->lastUse();
        return progress;
    }
}
