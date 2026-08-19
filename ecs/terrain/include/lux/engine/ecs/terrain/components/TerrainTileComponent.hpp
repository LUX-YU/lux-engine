#pragma once
/**
 * @file TerrainTileComponent.hpp
 * @brief Authored identity and content reference for one terrain tile.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/resource/spatial/Spatial.hpp>

namespace lux::ecs
{
    /// Placement comes from the entity's ordinary Transform3DComponent.  The
    /// integer coordinate is terrain-domain identity, not a Transform or
    /// memory-page representation.
    struct LUX_COMPONENT() TerrainTileComponent final
    {
        LUX_MEMBER(display_name=Coordinate)
        lux::spatial::GridCoord2i64 coordinate;

        LUX_MEMBER(display_name=Content,
                   readonly=true,
                   cooked_relocation=content_blob_ref)
        lux::ecs::scene_format::ContentBlobRef content;
    };
} // namespace lux::ecs
