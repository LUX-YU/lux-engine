#pragma once
#include <lux/engine/render/data/Annotations.hpp>

struct UnmarkedRecord
{
    float value;
};

struct LUX_RENDER_DATA(
    name = "unmarked.nested",
    version = 1,
    scope = "scene",
    authority = "cpu",
    gpu = "std430"
) InvalidNested
{
    UnmarkedRecord value;
};
