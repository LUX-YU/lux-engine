#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/packs/scene3d/animation_visibility.h>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kAnimation3DContributionName =
        "org.lux.builtin.animation3d";

    [[nodiscard]] LUX_ENGINE_PACK_ANIMATION3D_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeAnimation3DContribution(
        const lux::ecs::ComponentTypeCatalog& components);
}
