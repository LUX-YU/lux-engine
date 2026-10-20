#pragma once

#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    // Shared source/region boundary validation. Returns the last row's occupied byte extent,
    // excluding source_offset. No owning qualification or bypass flag is carried into an upload.
    [[nodiscard]] inline RenderResult<std::uint64_t> imageRegionBytes(
        const nri::TextureDesc& description,
        const ImageTransferRegion& region,
        std::uint64_t source_size,
        std::uint64_t chunk_bytes
    ) noexcept
    {
        const bool invalid_shape =
            !region.block_extent || !region.block_bytes || !region.texture.width || !region.texture.height ||
            region.texture.depth != 1 || region.texture.z != 0 || region.texture.mipOffset >= description.mipNum ||
            region.texture.planes != nri::PlaneBits::COLOR || region.texture.layerOffset >= description.layerNum;
        if (invalid_shape)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto width = std::max(1u, std::uint32_t(description.width) >> region.texture.mipOffset);
        const auto height = std::max(1u, std::uint32_t(description.height) >> region.texture.mipOffset);
        const bool invalid_offset = std::uint32_t(region.texture.x) > width || std::uint32_t(region.texture.y) > height;
        if (invalid_offset)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const bool invalid_extent =
            region.texture.width > width - region.texture.x || region.texture.height > height - region.texture.y;
        const bool invalid_blocks =
            std::uint32_t(region.texture.x) % region.block_extent ||
            std::uint32_t(region.texture.y) % region.block_extent ||
            (region.texture.width % region.block_extent && region.texture.width != width - region.texture.x) ||
            (region.texture.height % region.block_extent && region.texture.height != height - region.texture.y);
        const auto row_bytes = ((std::uint64_t(region.texture.width) + region.block_extent - 1) / region.block_extent) *
                               region.block_bytes;
        const auto row_count = (std::uint64_t(region.texture.height) + region.block_extent - 1) / region.block_extent;
        const bool invalid_pitch = region.source_row_pitch < row_bytes || row_bytes > chunk_bytes;
        if (invalid_extent || invalid_blocks || invalid_pitch)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto stride = cxx::checkedMul<std::uint64_t>(row_count - 1, region.source_row_pitch);
        if (!stride)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto bytes = cxx::checkedAdd(*stride, row_bytes);
        const bool invalid_source =
            !bytes || region.source_offset > source_size || *bytes > source_size - region.source_offset;
        if (invalid_source)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        return *bytes;
    }
} // namespace lux::render::detail
