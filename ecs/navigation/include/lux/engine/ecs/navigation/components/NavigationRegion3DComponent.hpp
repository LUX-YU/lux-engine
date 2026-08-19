#pragma once
/**
 * @file NavigationRegion3DComponent.hpp
 * @brief Authored reference to one region of navigation content.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>

namespace lux::ecs
{
    /// The entity fact contains no backend handle or lifecycle state.
    /// EntityScene relocates this reference to the Section-owned blob store.
    struct LUX_COMPONENT() NavigationRegion3DComponent final
    {
        LUX_MEMBER(display_name = Content,
                   readonly = true,
                   cooked_relocation = content_blob_ref)
        lux::ecs::scene_format::ContentBlobRef content;
    };
} // namespace lux::ecs
