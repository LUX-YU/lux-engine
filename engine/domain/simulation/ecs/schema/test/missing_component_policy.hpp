#pragma once

#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

struct LUX_COMPONENT(schema = "test.missing-policy", version = 1, semantic = DOMAIN_CONTRACT, editor = true)
    missing_component_policy final
{
    int value{};
};
