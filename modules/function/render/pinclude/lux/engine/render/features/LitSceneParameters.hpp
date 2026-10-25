#pragma once

#include <lux/engine/render/features/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/features/ShadowMapFeature.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>

namespace lux::render::detail
{
    struct LitSceneParameters final
    {
        MeshViewParameters mesh;
        DeferredLightingParameters lighting;
        TonemapParameters tone;
        std::array<float, 3> padding{};
        ShadowCamera shadow;
        ClusteredLightingParameters clusters;
    };

} // namespace lux::render::detail
