#pragma once

#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

struct LUX_COMPONENT(
    schema = "test.editor-hidden-component",
    version = 1,
    snapshot = REBUILD,
    semantic = RUNTIME_DERIVED,
    editor = false
) EditorHiddenComponent final
{
    double value{};
};
