#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/packs/scene2d/physics_visibility.h>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kPhysics2DContributionName =
        "org.lux.builtin.physics2d";
    inline constexpr std::string_view kDemoPhysics2DContributionName =
        "org.lux.builtin.physics2d_demo";

    [[nodiscard]] LUX_ENGINE_PACK_PHYSICS2D_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makePhysics2DContribution(
        const lux::ecs::ComponentTypeCatalog& components);

    [[nodiscard]] LUX_ENGINE_PACK_PHYSICS2D_PUBLIC
    SceneContributionDescriptor makeDemoPhysics2DContribution();
}
