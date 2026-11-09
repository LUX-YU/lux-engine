#include <lux/engine/render/core/Target.hpp>

namespace lux::render
{
    RenderResult<void> TargetDesc::validate() const noexcept
    {
        const bool is_invalid_extent = width == 0 || height == 0;
        if (is_invalid_extent)
        {
            return cxx::unexpected(RenderError{kInvalidTargetExtent, {width, height, 0}});
        }

        return validateEffectOutput({format, samples, kind, sampled});
    }

    RenderResult<void> validateEffectOutput(const EffectOutputDesc& output) noexcept
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

        const bool is_invalid_kind = output.kind != ETargetKind::OFFSCREEN && output.kind != ETargetKind::SURFACE;
        if (is_invalid_kind)
        {
            return cxx::unexpected(RenderError{kInvalidTargetKind, {static_cast<std::uint64_t>(output.kind), 0, 0}});
        }

        if (output.kind == ETargetKind::SURFACE && (output.samples != 1 || output.sampled))
        {
            return cxx::unexpected(RenderError{kInvalidTargetKind, {static_cast<std::uint64_t>(output.kind)}});
        }

        return {};
    }

} // namespace lux::render
