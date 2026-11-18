#pragma once

#include <lux/engine/render/capabilities/ColorEncoding.hpp>
#include <lux/engine/render/core/Target.hpp>

namespace lux::render
{
    struct TonemapFeatureConfig final
    {
        EffectOutputDesc output;
        EColorEncoding encoding = EColorEncoding::SRGB;
        bool operator==(const TonemapFeatureConfig&) const noexcept = default;
    };

} // namespace lux::render
