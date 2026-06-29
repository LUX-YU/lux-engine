#pragma once
#include <lux/engine/meta/LuxObject.hpp>

namespace lux::gameplay
{
    /// Links an entity to its parent in the scene hierarchy.
    /// Entities without this component are treated as root entities.
    struct HierarchyComponent
    {
        lux::meta::entity_id parent = lux::meta::null_entity;
    };

} // namespace lux::gameplay
