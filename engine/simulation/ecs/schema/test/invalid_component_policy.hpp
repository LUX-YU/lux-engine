#pragma once

#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

struct LUX_COMPONENT(schema = "test.invalid-policy", version = 1, snapshot = NEVER) invalid_component_policy final
{
    int value{};
};
