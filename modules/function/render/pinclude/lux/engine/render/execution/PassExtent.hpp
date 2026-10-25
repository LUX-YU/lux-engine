#pragma once

#include <algorithm>

#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    // One cold extent calculation for Attach and Resize. Lookup returns the actual resource slice;
    // Resize overlays only its changed resources, leaving unrelated Storage state untouched.
    template <class Lookup>
    [[nodiscard]] RenderResult<void> preparePassExtent(
        const DeviceSession& session,
        const RenderPlan& plan,
        const CompiledPass& compiled_pass,
        Lookup binding,
        std::array<std::uint32_t, 3>& dispatch_groups,
        std::vector<nri::TextureDataLayoutDesc>& copy_layouts
    ) noexcept
    {
        const auto& core = session.nriCore();
        if (const auto* compute = std::get_if<ComputeCommands>(&compiled_pass.work))
        {
            if (const auto* dispatch = std::get_if<ImageDispatchArguments>(&compute->dispatch))
            {
                const auto& image = core.GetTextureDesc(*binding(dispatch->resource.index).image);
                const auto width = std::max(1u, std::uint32_t(image.width) >> dispatch->mip);
                const auto height = std::max(1u, std::uint32_t(image.height) >> dispatch->mip);
                dispatch_groups =
                    {(width - 1) / dispatch->group_size[0] + 1, (height - 1) / dispatch->group_size[1] + 1, 1};
                const auto& limit = session.description().shaderStage.compute.dispatchMaxDim;
                if (dispatch_groups[0] > limit[0] || dispatch_groups[1] > limit[1])
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
        }

        if (const auto* raster = std::get_if<RasterCommands>(&compiled_pass.work))
        {
            const auto& reference = raster->colors.empty() ? *raster->depth : raster->colors.front();
            const auto& reference_range = std::get<GraphImageRange>(reference.access.range);
            const auto& reference_image = core.GetTextureDesc(*binding(reference.access.version.resource.index).image);
            const auto matchesExtent = [&](const RasterAttachment& input)
            {
                const auto& range = std::get<GraphImageRange>(input.access.range);
                const auto& image = core.GetTextureDesc(*binding(input.access.version.resource.index).image);
                return std::max(1u, std::uint32_t(image.width) >> range.first_mip) ==
                           std::max(1u, std::uint32_t(reference_image.width) >> reference_range.first_mip) &&
                       std::max(1u, std::uint32_t(image.height) >> range.first_mip) ==
                           std::max(1u, std::uint32_t(reference_image.height) >> reference_range.first_mip);
            };
            if (!std::ranges::all_of(raster->colors, matchesExtent) ||
                (raster->depth && !matchesExtent(*raster->depth)))
            {
                return cxx::unexpected(RenderError{kInvalidTargetExtent});
            }
        }
        if (const auto* transfer = std::get_if<TransferWork>(&compiled_pass.work))
        {
            copy_layouts.resize(transfer->operations.size());
            const auto regionValid = [&](const ResourceAccess& access, const std::optional<ImageCopyRegion>& rectangle)
            {
                if (!rectangle)
                {
                    return true;
                }
                const auto& range = std::get<GraphImageRange>(access.range);
                const auto& image = core.GetTextureDesc(*binding(access.version.resource.index).image);
                const auto format = imageFormat(
                    std::get<GraphImageDesc>(plan.resources()[access.version.resource.index].description).format
                );
                const auto width = std::max(1u, std::uint32_t(image.width) >> range.first_mip);
                const auto height = std::max(1u, std::uint32_t(image.height) >> range.first_mip);
                const bool invalid = !rectangle->width || !rectangle->height || range.mip_count != 1 ||
                                     std::uint64_t(rectangle->x) + rectangle->width > width ||
                                     std::uint64_t(rectangle->y) + rectangle->height > height ||
                                     rectangle->x % format.block || rectangle->y % format.block ||
                                     (rectangle->width % format.block && rectangle->x + rectangle->width != width) ||
                                     (rectangle->height % format.block && rectangle->y + rectangle->height != height);
                return !invalid;
            };

            for (std::size_t i = 0; i < transfer->operations.size(); ++i)
            {
                const auto& operation = transfer->operations[i];
                if (const auto* copy = std::get_if<CopyImage>(&operation))
                {
                    if (!regionValid(copy->source, copy->source_region) ||
                        !regionValid(copy->destination, copy->destination_region))
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                }
                else if (const auto* copy = std::get_if<CopyImageBuffer>(&operation))
                {
                    const bool source_image = std::holds_alternative<GraphImageRange>(copy->source.range);
                    const auto& image_access = source_image ? copy->source : copy->destination;
                    const auto& buffer_access = source_image ? copy->destination : copy->source;
                    const auto& image_range = std::get<GraphImageRange>(image_access.range);
                    const auto& buffer_range = std::get<GraphBufferRange>(buffer_access.range);
                    const auto& buffer = binding(buffer_access.version.resource.index);
                    const auto& image = core.GetTextureDesc(*binding(image_access.version.resource.index).image);
                    const auto format = imageFormat(
                        std::get<GraphImageDesc>(plan.resources()[image_access.version.resource.index].description)
                            .format
                    );
                    if (!regionValid(image_access, copy->image_region))
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const auto width = copy->image_region
                                           ? copy->image_region->width
                                           : std::max(1u, std::uint32_t(image.width) >> image_range.first_mip);
                    const auto height = copy->image_region
                                            ? copy->image_region->height
                                            : std::max(1u, std::uint32_t(image.height) >> image_range.first_mip);
                    const auto row_bytes = ((width + format.block - 1) / format.block) * format.bytes;
                    const auto rows = (height + format.block - 1) / format.block;
                    const auto& alignment = session.description().memoryAlignment;
                    const auto row = copy->row_pitch
                                         ? copy->row_pitch
                                         : (row_bytes + alignment.uploadBufferTextureRow - 1) /
                                               alignment.uploadBufferTextureRow * alignment.uploadBufferTextureRow;
                    const auto slice =
                        copy->slice_pitch ? copy->slice_pitch
                                          : (std::uint64_t(row) * rows + alignment.uploadBufferTextureSlice - 1) /
                                                alignment.uploadBufferTextureSlice * alignment.uploadBufferTextureSlice;
                    const bool invalid =
                        image_range.mip_count != 1 || image_range.layer_count != 1 || row < row_bytes ||
                        row % alignment.uploadBufferTextureRow || slice < std::uint64_t(row) * rows ||
                        slice > UINT32_MAX || slice % alignment.uploadBufferTextureSlice || slice > buffer_range.size ||
                        (buffer.buffer_offset + buffer_range.offset) % alignment.uploadBufferTextureSlice;
                    if (invalid)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    copy_layouts[i] = {buffer_range.offset, row, static_cast<std::uint32_t>(slice)};
                }
            }
        }
        return {};
    }
} // namespace lux::render::detail
