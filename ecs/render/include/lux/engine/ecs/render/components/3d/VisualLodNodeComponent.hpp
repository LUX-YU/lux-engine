#pragma once
/**
 * @file VisualLodNodeComponent.hpp
 * @brief Presentation LOD facts, independent of entity transform hierarchy.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/entity_scene/EntitySceneIdentifiers.hpp>

#include <cstdint>

namespace lux::ecs
{
    struct LUX_COMPONENT() VisualLodNodeComponent final
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

    /// Optional normalized hierarchy edge.  Root nodes omit this component;
    /// children are derived by a domain index instead of being duplicated in
    /// every parent component.  It is intentionally unrelated to
    /// ParentComponent, which owns transform composition.
    struct LUX_COMPONENT() VisualLodParentComponent final
    {
        LUX_MEMBER(display_name=Parent,
                   readonly=true,
                   cooked_relocation=persistent_entity_ref)
        lux::entity_scene::PersistentEntityRef parent;
    };
} // namespace lux::ecs
