#pragma once

#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::simulation::ecs
{
    struct LUX_COMPONENT(schema = "lux.ecs.Parent", version = 1, snapshot = COPY) Parent final
    {
        Entity entity{NullEntity};
    };
} // namespace lux::simulation::ecs

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/simulation/ecs/Parent.type_static_info.hpp>
#endif
