#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/packs/scene3d/physics_visibility.h>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DPrepareService.hpp>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kPhysics3DContributionName =
        "org.lux.builtin.physics3d";

    [[nodiscard]] LUX_ENGINE_PACK_PHYSICS3D_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makePhysics3DContribution(
        const lux::ecs::ComponentTypeCatalog& components,
        spatial3d::StaticCollider3DPrepareClient preparation);
}
