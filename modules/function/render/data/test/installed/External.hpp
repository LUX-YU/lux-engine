#pragma once
#include <lux/engine/render/data/Annotations.hpp>

struct LUX_RENDER_DATA(
    name = "external.sdk.record",
    version = 1,
    scope = "scene",
    authority = "cpu",
    gpu = "std140"
) External
{
    float values[3];
};
