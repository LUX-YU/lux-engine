#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <string>

namespace lux::gameplay
{
    /// Human-readable name attached to an entity, primarily for editor
    /// display (the Hierarchy panel uses it as the row label and the
    /// Inspector exposes it as an editable text field).
    struct LUX_COMPONENT() NameComponent
    {
        LUX_MEMBER(display_name=Name, tooltip=Editor display label for this entity)
        std::string name;
    };

} // namespace lux::gameplay
