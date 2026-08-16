#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/packs/scene2d/presentation_visibility.h>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kPresentation2DContributionName =
        "org.lux.builtin.presentation2d";

    [[nodiscard]] LUX_ENGINE_PACK_PRESENTATION2D_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makePresentation2DContribution(
        const lux::ecs::ComponentTypeCatalog& components);
}
