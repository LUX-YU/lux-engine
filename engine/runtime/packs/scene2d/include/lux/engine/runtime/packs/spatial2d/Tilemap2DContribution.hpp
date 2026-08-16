#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/spatial2d/tilemap/TilemapPrepareService.hpp>
#include <lux/engine/runtime/packs/scene2d/tilemap_visibility.h>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kTilemap2DContributionName =
        "org.lux.builtin.tilemap2d";

    [[nodiscard]] LUX_ENGINE_PACK_TILEMAP2D_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeTilemap2DContribution(
        const lux::ecs::ComponentTypeCatalog& components,
        spatial2d::TilemapPrepareClient preparation);
} // namespace lux::runtime
