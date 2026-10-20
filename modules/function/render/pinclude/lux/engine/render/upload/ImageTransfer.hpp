#pragma once

#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/upload/UploadContext.hpp>

namespace lux::render::detail
{
    // Shared source/region boundary validation. Returns the last row's occupied byte extent,
    // excluding source_offset. No owning qualification or bypass flag is carried into an upload.
    [[nodiscard]] inline RenderResult<std::uint64_t> imageRegionBytes(
        VkExtent3D extent,
        std::uint32_t mips,
        std::uint32_t layers,
        const ImageTransferRegion& region,
        std::uint64_t source_size,
        std::uint64_t chunk_bytes
    ) noexcept
    {
        const bool invalid_shape = !region.block_extent || !region.block_bytes || !region.extent.width ||
                                   !region.extent.height || region.extent.depth != 1 || region.offset.x < 0 ||
                                   region.offset.y < 0 || region.offset.z != 0 || region.subresource.mipLevel >= mips ||
                                   region.subresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                                   region.subresource.layerCount != 1 || region.subresource.baseArrayLayer >= layers;
        if (invalid_shape)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto width = std::max(1u, extent.width >> region.subresource.mipLevel);
        const auto height = std::max(1u, extent.height >> region.subresource.mipLevel);
        const bool invalid_offset = std::uint32_t(region.offset.x) > width || std::uint32_t(region.offset.y) > height;
        if (invalid_offset)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const bool invalid_extent =
            region.extent.width > width - region.offset.x || region.extent.height > height - region.offset.y;
        const bool invalid_blocks =
            std::uint32_t(region.offset.x) % region.block_extent ||
            std::uint32_t(region.offset.y) % region.block_extent ||
            (region.extent.width % region.block_extent && region.extent.width != width - region.offset.x) ||
            (region.extent.height % region.block_extent && region.extent.height != height - region.offset.y);
        const auto row_bytes =
            ((std::uint64_t(region.extent.width) + region.block_extent - 1) / region.block_extent) * region.block_bytes;
        const auto row_count = (std::uint64_t(region.extent.height) + region.block_extent - 1) / region.block_extent;
        const bool invalid_pitch = region.source_row_pitch < row_bytes || row_bytes > chunk_bytes;
        if (invalid_extent || invalid_blocks || invalid_pitch)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto stride = cxx::checkedMul<std::uint64_t>(row_count - 1, region.source_row_pitch);
        if (!stride)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto bytes = cxx::checkedAdd(*stride, row_bytes);
        const bool invalid_source =
            !bytes || region.source_offset > source_size || *bytes > source_size - region.source_offset;
        if (invalid_source)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        return *bytes;
    }
} // namespace lux::render::detail
