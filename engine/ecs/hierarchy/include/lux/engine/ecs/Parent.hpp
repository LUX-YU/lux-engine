#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{
    struct LUX_COMPONENT(
        schema = "lux.ecs.Parent",
        version = 1,
        snapshot = COPY,
        section = LOAD
    ) Parent final
    {
        Entity entity{NullEntity};
    };
} // namespace lux::ecs

#if !defined(__LUX_PARSE_TIME__)
#    include <lux/engine/ecs/Parent.type_static_info.hpp>
#endif
