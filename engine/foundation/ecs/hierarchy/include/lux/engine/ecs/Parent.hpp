#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/ecs/Entity.hpp>

namespace lux::ecs
{
    struct LUX_COMPONENT(
        schema = "lux.ecs.Parent", version = 1, snapshot = COPY, persistent = true, semantic = FOUNDATION
    ) Parent final
    {
        Entity entity{NullEntity};
    };
} // namespace lux::ecs

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/ecs/Parent.type_static_info.hpp>
#endif
