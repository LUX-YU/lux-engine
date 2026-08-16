#pragma once
/**
 * @file TerrainLodNodeComponent.hpp
 * @brief Terrain-specific LOD facts and normalized parent edge.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/entity_scene/EntitySceneIdentifiers.hpp>

#include <cstdint>

namespace lux::ecs
{
    struct LUX_COMPONENT() TerrainLodNodeComponent final
    {
        LUX_MEMBER(display_name=Level)
        std::uint8_t level{0u};

        LUX_MEMBER(display_name=Geometric Error, min=0.0)
        float geometric_error{0.0f};

        LUX_MEMBER(display_name=Enter Error Pixels, min=0.0)
        float enter_error_pixels{2.5f};

        LUX_MEMBER(display_name=Exit Error Pixels, min=0.0)
        float exit_error_pixels{1.5f};
    };

    /// Root tiles omit this component.  Children are derived from these edges
    /// so cooked content has one LOD-topology truth rather than mirrored
    /// parent and children arrays.
    struct LUX_COMPONENT() TerrainLodParentComponent final
    {
        LUX_MEMBER(display_name=Parent,
                   readonly=true,
                   cooked_relocation=persistent_entity_ref)
        lux::entity_scene::PersistentEntityRef parent;
    };
} // namespace lux::ecs
