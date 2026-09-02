#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

namespace lux::editor::inspector::test
{
    struct LUX_COMPONENT(
        schema = "test.plugin-component",
        version = 1,
        snapshot = COPY,
        semantic = IMPLEMENTATION_EXTENSION,
        editor = true
    ) PluginComponent final
    {
        double LUX_MEMBER(display_name = Value, widget = slider, min = -4.0, max = 4.0, speed = 0.25) value{};
    };
}
