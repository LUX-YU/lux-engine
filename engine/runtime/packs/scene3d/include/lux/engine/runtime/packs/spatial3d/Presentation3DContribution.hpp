#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/packs/scene3d/presentation_visibility.h>
#include <lux/engine/runtime/render/scene/SceneGeometryPrepareService.hpp>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kPresentation3DContributionName =
        "org.lux.builtin.presentation3d";

    [[nodiscard]] LUX_ENGINE_PACK_PRESENTATION3D_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makePresentation3DContribution(
        const lux::ecs::ComponentTypeCatalog& components,
        ClassicMeshPrepareClient classic_mesh_preparation,
        TerrainPrepareClient terrain_preparation);
}
