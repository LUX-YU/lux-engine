#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>

struct LUX_COMPONENT(
    schema = "test.missing-policy",
    version = 1
) missing_component_policy final
{
    int value{};
};
