#pragma once

#include <lux/engine/render/features/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/features/ShadowMapFeature.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>

namespace lux::scene::detail
{
    struct SceneParameters final
    {
        render::MeshViewParameters mesh;
        render::DeferredLightingParameters lighting;
        render::TonemapParameters tone;
        std::array<float, 3> padding{};
        render::ShadowCamera shadow;
        render::ClusteredLightingParameters clusters;
    };

} // namespace lux::scene::detail
