#pragma once

#include <lux/engine/render/graph/execute/CommandExtent.hpp>

#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{
    // One cold extent calculation for Attach and Resize. Lookup returns the actual resource slice;
    // Resize overlays only its changed resources, leaving unrelated Storage state untouched.
    template <class Lookup>
    [[nodiscard]] RenderResult<void>
    validatePassExtent(
        const DeviceSession& session,
        const RenderGraphExecutable& plan,
        const CompiledPass& pass,
        Lookup binding
    ) noexcept
    {
        const auto& core = session.nriCore();
        if (const auto* compute = plan.computeCommands(pass))
        {
            if (const auto* dispatch = std::get_if<ImageDispatchExtent>(&compute->dispatch))
            {
                const auto* image = binding(dispatch->image).image;
                if (!image) return cxx::unexpected(RenderError{kDeviceArgument});
                const auto& desc = core.GetTextureDesc(*image);
                auto groups = dispatchGroups(*dispatch, {desc.width, desc.height}, session.description());
                if (!groups) return cxx::unexpected(groups.error());
            }
        }

        if (const auto* raster = pass.rasterScope())
        {
            const auto& reference = raster->colors.empty() ? *raster->depth : raster->colors.front();
            const auto& reference_range = std::get<GraphImageRange>(reference.access.range);
            const auto& reference_image = core.GetTextureDesc(*binding(reference.access.version.resource.index).image);
            const auto matchesExtent = [&](const ResourceAccess& access)
            {
                const auto& range = std::get<GraphImageRange>(access.range);
                const auto& image = core.GetTextureDesc(*binding(access.version.resource.index).image);
                return std::max(1u, std::uint32_t(image.width) >> range.first_mip) ==
                           std::max(1u, std::uint32_t(reference_image.width) >> reference_range.first_mip) &&
                       std::max(1u, std::uint32_t(image.height) >> range.first_mip) ==
                           std::max(1u, std::uint32_t(reference_image.height) >> reference_range.first_mip);
            };
            const auto attachmentFits = [&](const RasterAttachment& attachment) noexcept
            {
                return matchesExtent(attachment.access) &&
                    (!attachment.resolve || matchesExtent(*attachment.resolve));
            };
            if (!std::ranges::all_of(raster->colors, attachmentFits) ||
                (raster->depth && !attachmentFits(*raster->depth)))
            {
                return cxx::unexpected(RenderError{kInvalidTargetExtent});
            }
        }
        if (const auto* transfer = plan.transferCommands(pass))
        {
            const auto resolve = [&](const ImageOperand& operand) noexcept -> RenderResult<nri::TextureRegionDesc>
            {
                const auto* image = binding(operand.slot).image;
                if (!image) return cxx::unexpected(RenderError{kDeviceArgument});
                const auto& desc = core.GetTextureDesc(*image);
                return copyRegion(operand, {desc.width, desc.height});
            };
            for (const auto& packet : transfer->operations)
            {
                if (const auto* copy = std::get_if<ImageCopyPacket>(&packet))
                {
                    auto from = resolve(copy->source);
                    if (!from) return cxx::unexpected(from.error());
                    auto to = resolve(copy->destination);
                    if (!to) return cxx::unexpected(to.error());
                    if (from->width != to->width || from->height != to->height)
                        return cxx::unexpected(RenderError{kInvalidTargetExtent});
                }
                else if (const auto* mixed = std::get_if<ImageBufferPacket>(&packet))
                {
                    auto region = resolve(mixed->image);
                    if (!region) return cxx::unexpected(region.error());
                    const auto& alignment = session.description().memoryAlignment;
                    auto layout = copyLayout(
                        *mixed, *region, binding(mixed->buffer.slot).buffer_offset,
                        {alignment.uploadBufferTextureRow, alignment.uploadBufferTextureSlice}
                    );
                    if (!layout) return cxx::unexpected(layout.error());
                }
            }
        }
        return {};
    }
} // namespace lux::render::detail
