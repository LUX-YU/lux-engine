#include <lux/engine/render/graph/GraphOutput.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render
{
    RenderResult<void> validateGraphOutput(const GraphOutputRequirements& output) noexcept
    {
        const bool is_invalid_format =
            !rdesc::supportsTextureUsage(output.format, rdesc::ETextureUsage::COLOR_ATTACHMENT);
        if (is_invalid_format)
        {
            return cxx::unexpected(RenderError{
                kInvalidTargetFormat, {static_cast<std::uint64_t>(output.format), 0, 0}
            });
        }

        const bool is_valid_samples =
            output.samples != 0 && output.samples <= 64 && (output.samples & (output.samples - 1)) == 0;
        if (!is_valid_samples)
        {
            return cxx::unexpected(RenderError{kInvalidTargetSamples, {output.samples, 0, 0}});
        }

        if ((output.width == 0) != (output.height == 0))
            return cxx::unexpected(RenderError{kInvalidTargetExtent, {output.width, output.height, 0}});
        return {};
    }
}

namespace lux::render::compiled
{
    bool RenderGraph::compatible(const GraphOutputRequirements& target) const noexcept
    {
        const bool same_image = target.format == output_description_.format && target.samples == output_description_.samples;
        const bool same_extent = output_description_.width == 0 ||
            (target.width == output_description_.width && target.height == output_description_.height);
        return same_image && same_extent;
    }
}
