#pragma once

#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

struct LUX_COMPONENT(schema = "test.duplicate-policy", version = 1, snapshot = COPY, snapshot = REBUILD,
                     semantic = DOMAIN_CONTRACT, editor = true)
    duplicate_component_policy final
{
    int value{};
};
