#pragma once
#include <lux/engine/render/data/Annotations.hpp>

struct LUX_RENDER_DATA(name = "bad", version = 1, scope = "scene", authority = "cpu", gpu = "std430") Bad
{
    float* value;
};
