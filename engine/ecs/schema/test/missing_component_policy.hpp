#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>

struct LUX_COMPONENT_SCHEMA("test.missing-policy", 1)
    missing_component_policy final
{
    int value{};
};
