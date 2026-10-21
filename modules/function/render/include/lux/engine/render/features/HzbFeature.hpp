#pragma once

#include <lux/engine/render/features/FeatureDescriptor.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    // Forward ZO depth: near=0, far/clear=1. A pyramid texel stores the farthest covered depth.
    struct HzbFeatureConfig final
    {
        GraphImageDesc depth;
        std::uint32_t mip_count = 1;
    };

    struct HzbFeatureDescriptor final
    {
        using Config = HzbFeatureConfig;
        static inline constexpr FeatureName name{"lux.render.feature.Hzb"};
        LUX_RENDER_PUBLIC static const FeatureDescriptor descriptor;
    };
} // namespace lux::render
