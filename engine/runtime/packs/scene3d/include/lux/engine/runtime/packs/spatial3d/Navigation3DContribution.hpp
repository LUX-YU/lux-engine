#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/packs/scene3d/navigation_visibility.h>
#include <lux/engine/runtime/spatial3d/navigation/Navigation3DPrepareService.hpp>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kNavigation3DContributionName =
        "org.lux.builtin.navigation3d";

    [[nodiscard]] LUX_ENGINE_PACK_NAVIGATION3D_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeNavigation3DContribution(
        const lux::ecs::ComponentTypeCatalog& components,
        spatial3d::Navigation3DPrepareClient preparation);
}
