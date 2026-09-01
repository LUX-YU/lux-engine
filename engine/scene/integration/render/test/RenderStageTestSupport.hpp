#pragma once

#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>

#include <algorithm>
#include <array>
#include <cassert>

namespace lux::scene::test
{
    inline constexpr render::FeatureTypeId MeshFeature = render::featureId("lux.render.mesh_stack.v1");
    inline constexpr render::FeatureTypeId LightFeature = render::featureId("lux.render.light.v1");

    [[nodiscard]] inline render::FeatureCatalog makeFeatureCatalog(
        std::span<const render::TypeId> mesh_ops,
        std::span<const render::TypeId> light_ops
    )
    {
        render::FeatureCatalog catalog;
        render::FeatureFactory mesh{};
        mesh.name = "MeshStack";
        mesh.descriptor = render::FeatureDescriptor{.type = MeshFeature, .name = "MeshStack"};
        catalog.add(mesh, 1U, mesh_ops);
        render::FeatureFactory light{};
        light.name = "Light";
        light.descriptor = render::FeatureDescriptor{.type = LightFeature, .name = "Light"};
        catalog.add(light, 2U, light_ops);
        return catalog;
    }

    [[nodiscard]] inline const RenderFeatureSceneBinding& binding(render::FeatureTypeId type)
    {
        const auto bindings = builtinRenderFeatureSceneBindings();
        const auto found = std::find_if(bindings.begin(), bindings.end(), [type](const auto& value) noexcept {
            return value.feature == type;
        });
        assert(found != bindings.end());
        return *found;
    }

    [[nodiscard]] inline lux::cxx::expected<std::unique_ptr<RenderSyncStage>, RenderSyncStageCreateFailure>
    createStage(
        render::FeatureTypeId type,
        simulation::ecs::Registry& registry,
        render::RenderSceneId scene,
        const render::FeatureCatalog& catalog,
        double coordinate_page_size = 1024.0,
        std::array<std::int64_t, 3> scene_origin_page = {}
    ) noexcept
    {
        const RenderSyncStageCreateInfo info{
            registry,
            scene,
            catalog,
            type,
            render::FeatureHandle{1U, 1U},
            coordinate_page_size,
            scene_origin_page
        };
        return binding(type).create_sync_stage(info);
    }
} // namespace lux::scene::test
