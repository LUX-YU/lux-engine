#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/packs/scene2d/transform_visibility.h>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kSpatial2DTransformContributionName =
        "org.lux.builtin.spatial2d.transform";

    [[nodiscard]] LUX_ENGINE_PACK_SPATIAL2D_TRANSFORM_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeSpatial2DTransformContribution(
        const lux::ecs::ComponentTypeCatalog& components);
}
