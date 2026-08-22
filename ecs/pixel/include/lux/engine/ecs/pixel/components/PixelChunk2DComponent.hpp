#pragma once
/**
 * @file PixelChunk2DComponent.hpp
 * @brief Authored identity and content reference for one sparse pixel chunk.
 */

#include <lux/engine/ecs/PersistentEntityId.hpp>
#include <lux/engine/ecs/reflection/SpatialValueReflectionTraits.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/math/Grid.hpp>

namespace lux::ecs
{
    /// Runtime handles, active/resident state and journal ownership belong to
    /// PixelFieldSystem.  This component remains valid while its field entity
    /// is unloaded: `field` is resolved lazily through PersistentEntityIndex.
    struct LUX_COMPONENT() PixelChunk2DComponent final
    {
        LUX_MEMBER(display_name=Coordinate)
        lux::math::GridCoord2i64 coordinate;

        // These two leaves are filled by generic LXES relocation tables. They
        // deliberately remain reflected fields even though TaggedProperty
        // serialization has no raw-object representation for either type.
        LUX_MEMBER(display_name=Field, readonly=true, cooked_relocation=persistent_entity_ref)
        PersistentEntityRef field;

        LUX_MEMBER(display_name=Content, readonly=true, cooked_relocation=content_blob_ref)
        lux::ecs::scene_format::ContentBlobRef content;
    };
}
