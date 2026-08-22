#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/runtime/spatial2d/tilemap/TilemapPrepareService.hpp>
#include <lux/engine/runtime/scene/composition/tilemap_visibility.h>

namespace lux::runtime
{
    [[nodiscard]] LUX_ENGINE_TILEMAP_SYSTEMS_PUBLIC
    bool installTilemap2DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        spatial2d::TilemapPrepareClient preparation);
} // namespace lux::runtime
