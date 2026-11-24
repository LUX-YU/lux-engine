#pragma once

#include <lux/engine/render/graph/CommandPackets.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <algorithm>

namespace lux::render::detail
{
    struct ImageDimensions final { std::uint32_t width, height, depth = 1; };
    struct CopyAlignment final { std::uint32_t row, slice; };

    [[nodiscard]] inline RenderResult<nri::DispatchDesc>
    dispatchGroups(const ImageDispatchExtent& dispatch, ImageDimensions extent, const nri::DeviceDesc& device) noexcept
    {
        if (!extent.width || !extent.height || !extent.depth) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto groupsFor = [&](std::uint32_t size, std::uint32_t group_size) noexcept
        {
            return (std::max(1u, size >> dispatch.mip) - 1) / group_size + 1;
        };
        const nri::DispatchDesc groups{
            groupsFor(extent.width, dispatch.group_size[0]), groupsFor(extent.height, dispatch.group_size[1]),
            groupsFor(extent.depth, dispatch.group_size[2])
        };
        const auto& limit = device.shaderStage.compute.dispatchMaxDim;
        if (groups.workGroupNumX > limit[0] || groups.workGroupNumY > limit[1] || groups.workGroupNumZ > limit[2])
            return cxx::unexpected(RenderError{kDeviceArgument});
        return groups;
    }

    [[nodiscard]] inline RenderResult<nri::TextureRegionDesc>
    copyRegion(const ImageOperand& image, ImageDimensions extent) noexcept
    {
        const auto& range = image.range;
        if (!extent.width || !extent.height) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto width = std::max(1u, extent.width >> range.first_mip);
        const auto height = std::max(1u, extent.height >> range.first_mip);
        const auto depth = std::max(1u, extent.depth >> range.first_mip);
        const auto rect = image.region.value_or(ImageCopyRegion{0, 0, width, height, 0, depth});
        const bool fits = rect.width && rect.height && std::uint64_t(rect.x) + rect.width <= width &&
            std::uint64_t(rect.y) + rect.height <= height && rect.depth && std::uint64_t(rect.z) + rect.depth <= depth;
        const auto block = image.block_dim;
        const bool aligned = rect.x % block == 0 && rect.y % block == 0 &&
            (rect.width % block == 0 || rect.x + rect.width == width) &&
            (rect.height % block == 0 || rect.y + rect.height == height);
        if (!fits || !aligned || (image.region && range.mip_count != 1))
            return cxx::unexpected(RenderError{kDeviceArgument});

        const auto planes = range.aspects == 1 ? nri::PlaneBits::COLOR :
            range.aspects == 2 ? nri::PlaneBits::DEPTH : nri::PlaneBits::STENCIL;
        using Dim = nri::Dim_t;
        return nri::TextureRegionDesc{
            Dim(rect.x), Dim(rect.y), Dim(rect.z), Dim(rect.width), Dim(rect.height), Dim(rect.depth),
            Dim(range.first_mip), Dim(range.first_layer), planes
        };
    }

    [[nodiscard]] inline RenderResult<nri::TextureDataLayoutDesc>
    copyLayout(
        const ImageBufferPacket& copy,
        const nri::TextureRegionDesc& region,
        std::uint64_t buffer_base,
        CopyAlignment alignment
    ) noexcept
    {
        const auto block = copy.image.block_dim;
        const auto rows = (std::uint32_t(region.height) + block - 1) / block;
        const auto columns = (std::uint32_t(region.width) + block - 1) / block;
        const auto minimum_row = std::uint64_t(columns) * copy.image.block_bytes;
        const auto align = [](std::uint64_t n, std::uint32_t to) noexcept { return (n + to - 1) / to * to; };
        const auto row = copy.row_pitch ? copy.row_pitch : align(minimum_row, alignment.row);
        const auto min_slice = row * rows;
        const auto slice = copy.slice_pitch ? copy.slice_pitch : align(min_slice, alignment.slice);
        const auto& range = copy.buffer.range;
        const bool valid_row = row >= minimum_row && row <= UINT32_MAX && row % alignment.row == 0 && row % copy.image.block_bytes == 0;
        const bool valid_slice = slice >= min_slice && slice <= UINT32_MAX && slice * region.depth <= range.size &&
            slice % alignment.slice == 0;
        const bool valid_offset = range.offset <= UINT64_MAX - buffer_base &&
            (buffer_base + range.offset) % alignment.slice == 0;
        if (!valid_row || !valid_slice || !valid_offset) return cxx::unexpected(RenderError{kDeviceArgument});
        return nri::TextureDataLayoutDesc{buffer_base + range.offset, std::uint32_t(row), std::uint32_t(slice)};
    }
}
