#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

struct LUX_COMPONENT(
    schema = "test.invalid-editor-widget",
    version = 1,
    snapshot = COPY,
    semantic = IMPLEMENTATION_EXTENSION,
    editor = true
) InvalidEditorWidgetComponent final
{
    double LUX_MEMBER(widget = backend_escape) value{};
};
