#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>

struct LUX_COMPONENT(
    schema = "test.duplicate-policy",
    version = 1,
    snapshot = COPY,
    snapshot = REBUILD,
    section = LOAD
) duplicate_component_policy final
{
    int value{};
};
