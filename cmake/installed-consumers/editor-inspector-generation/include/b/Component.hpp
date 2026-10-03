#pragma once
#include <Common.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
namespace b
{
    struct LUX_COMPONENT(
        schema = "incremental.b",
        version = 1,
        snapshot = COPY,
        semantic = DOMAIN_CONTRACT,
        editor = true
    ) Component final
    {
        Scalar LUX_MEMBER(widget = slider, min = 0, max = 10) value{2};
    };
} // namespace b
#if !defined(__LUX_PARSE_TIME__)
#include <b/Component.type_static_info.hpp>
#endif
