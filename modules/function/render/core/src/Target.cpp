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

        const bool is_invalid_format = !rdesc::supportsTextureUsage(format, rdesc::ETextureUsage::COLOR_ATTACHMENT);
        if (is_invalid_format)
        {
            return cxx::unexpected(RenderError{kInvalidTargetFormat, {static_cast<std::uint64_t>(format), 0, 0}});
        }

        const bool is_valid_samples = samples != 0 && samples <= 64 && (samples & (samples - 1)) == 0;
        if (!is_valid_samples)
        {
            return cxx::unexpected(RenderError{kInvalidTargetSamples, {samples, 0, 0}});
        }

        const bool is_invalid_kind = kind != ETargetKind::OFFSCREEN && kind != ETargetKind::SURFACE;
        if (is_invalid_kind)
        {
            return cxx::unexpected(RenderError{kInvalidTargetKind, {static_cast<std::uint64_t>(kind), 0, 0}});
        }

        return {};
    }

    std::span<const error::ErrorDescriptor> renderCoreErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {
                "lux.render.core.invalid_target_extent", "Target width and height must be nonzero",
                error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            {
                "lux.render.core.invalid_target_format", "Target format must describe an uncompressed color attachment",
                error::ERecovery::PERMANENT, {error::EArgument::HEX}
            },
            {
                "lux.render.core.invalid_target_samples", "Target samples must be a power of two from 1 through 64",
                error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED}
            },
            {
                "lux.render.core.invalid_target_kind", "Unknown target kind",
                error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED}
            },
            {
                "lux.render.core.invalid_viewport", "Raster viewport coordinates and depth ranges must be valid",
                error::ERecovery::PERMANENT, {}
            }
        };
        return descriptors;
    }
}
