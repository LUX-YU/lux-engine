#pragma once

#include <lux/engine/ecs/PersistentEntityId.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/entity_scene/EntitySection.hpp>
#include <lux/engine/resource/spatial/Spatial.hpp>

namespace lux::ecs
{
    /// Authored identity of one independently resident tilemap chunk.
    /// Runtime handles and activity belong to the Tilemap ECS consumer; this
    /// component contains no partition or presentation policy.
    struct LUX_COMPONENT() TileChunk2DComponent final
    {
        LUX_MEMBER(display_name=Coordinate)
        lux::spatial::GridCoord2i64 coordinate;

        LUX_MEMBER(display_name=Tilemap, readonly=true, cooked_relocation=persistent_entity_ref)
        PersistentEntityRef tilemap;

        LUX_MEMBER(display_name=Content, readonly=true, cooked_relocation=content_blob_ref)
        lux::entity_scene::ContentBlobRef content;
    };
}
