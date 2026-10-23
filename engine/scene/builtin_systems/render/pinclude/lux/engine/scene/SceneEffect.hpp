#pragma once

#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/features/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/ShadowMapFeature.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>

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

    struct SceneEffectFeatures final
    {
        render::FeatureId gbuffer, lighting, tonemap, visibility, shadows, mesh_shadow, clusters;
    };

    // Pure cold recipe construction. No Scene, Requester or execution lifetime is retained.
    [[nodiscard]] render::RenderResult<render::CreateEffectRequest> makeSceneEffect(
        render::TargetDesc description,
        std::vector<render::RasterMeshGroup> groups,
        std::uint32_t visible_capacity,
        SceneEffectFeatures features,
        const RenderSystemConfiguration& configuration,
        std::vector<render::ShadowLightSelection> casters
    ) noexcept;
} // namespace lux::scene::detail
