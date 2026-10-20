#pragma once

#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    struct MaterialMeshRasterDescriptor final
    {
        using Config = MeshFeatureConfig;
        static inline constexpr FeatureName name{"lux.render.feature.MaterialMeshRaster"};
        LUX_RENDER_PUBLIC static const FeatureDescriptor descriptor;
    };
} // namespace lux::render
