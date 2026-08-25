#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{
    struct LUX_TYPE_INFO(static)
        LUX_COMPONENT_SCHEMA("lux.ecs.Parent", 1)
        LUX_COMPONENT_SNAPSHOT(COPY)
        LUX_COMPONENT_WORLD_SECTION(LOAD) Parent final
    {
        Entity entity{NullEntity};
    };
} // namespace lux::ecs

#if !defined(__LUX_PARSE_TIME__)
#    include <lux/engine/ecs/Parent.type_static_info.hpp>
#endif
