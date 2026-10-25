#pragma once

#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>

namespace lux::render
{
    enum class EColorEncoding : std::uint8_t
    {
        LINEAR,
        SRGB
    };

    struct TonemapParameters final
    {
        float exposure = 0;
    }; // Exposure value in stops: multiply linear HDR by exp2(exposure).

    [[nodiscard]] inline rdesc::ShaderBlock tonemapParameterBlock() noexcept
    {
        return {
            sizeof(TonemapParameters),
            4,
            rdesc::EDataLayout::STD430,
            {{"exposure", rdesc::EDataScalar::FLOAT, 0, 1, 1, 0, 0}}
        };
    }

    struct TonemapFeatureConfig final
    {
        TargetDesc target;
        EColorEncoding encoding = EColorEncoding::SRGB;
        rdesc::ShaderBlock input_layout = tonemapParameterBlock();
        bool operator==(const TonemapFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<TonemapFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const TonemapFeatureConfig&) noexcept;
    };

} // namespace lux::render
