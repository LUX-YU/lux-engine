#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/packs/scene2d/simulation_visibility.h>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kSimulation2DContributionName =
        "org.lux.builtin.simulation2d";

    [[nodiscard]] LUX_ENGINE_PACK_SIMULATION2D_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeSimulation2DContribution(
        const lux::ecs::ComponentTypeCatalog& components);
}
