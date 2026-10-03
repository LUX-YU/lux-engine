#pragma once
#include <Common.hpp>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
namespace a
{
    struct LUX_COMPONENT(schema = "incremental.a", version = 1, snapshot = COPY, semantic = DOMAIN_CONTRACT, editor = true)
    Component final
    {
        Scalar LUX_MEMBER(widget = slider, min = 0, max = 10) value{1};
    };
}
#if !defined(__LUX_PARSE_TIME__)
#include <a/Component.type_static_info.hpp>
#endif
