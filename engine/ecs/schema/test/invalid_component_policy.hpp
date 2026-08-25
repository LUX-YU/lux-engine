#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>

struct LUX_COMPONENT(
    schema = "test.invalid-policy",
    version = 1,
    snapshot = NEVER,
    section = LOAD
) invalid_component_policy final
{
    int value{};
};
