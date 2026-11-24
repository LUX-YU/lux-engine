#include <lux/engine/render/graph/GraphOutput.hpp>
#include <lux/engine/render/runtime/Target.hpp>

namespace lux::render
{
    RenderResult<void> TargetDesc::validate() const noexcept
    {
        const bool is_invalid_extent = width == 0 || height == 0;
        if (is_invalid_extent)
        {
            return cxx::unexpected(RenderError{kInvalidTargetExtent, {width, height, 0}});
        }

        const bool invalid_mode = display_mode != EDisplayMode::SDR && display_mode != EDisplayMode::HDR;
        if (invalid_mode || (display_mode == EDisplayMode::HDR && kind != ETargetKind::SURFACE))
            return cxx::unexpected(RenderError{kInvalidTargetKind, {static_cast<std::uint64_t>(kind)}});
        return validateEffectOutput(outputContract());
    }

    RenderResult<void> validateEffectOutput(const EffectOutputDesc& output) noexcept
    {
        if (auto valid = validateGraphOutput({output.format, output.samples}); !valid)
            return valid;

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
