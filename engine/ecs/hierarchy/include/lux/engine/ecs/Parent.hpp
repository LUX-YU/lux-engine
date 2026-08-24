#pragma once

#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{
    struct LUX_COMPONENT_SCHEMA("lux.ecs.Parent", 1) Parent final
    {
        Entity entity{NullEntity};
    };
} // namespace lux::ecs
