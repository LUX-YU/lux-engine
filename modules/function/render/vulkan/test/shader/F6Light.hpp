#pragma once
#include <lux/engine/render/data/Annotations.hpp>

struct LUX_DATA_VALUE() F6Position
{
    float LUX_GPU_FIELD(shape = "vec3") xyz[3];
    float intensity;
};

struct LUX_RENDER_DATA(
    name = "lux.test.f6.light",
    version = 1,
    scope = "scene",
    authority = "cpu",
    gpu = "std430"
) LightGpu
{
    F6Position position;
    float LUX_GPU_FIELD(shape = "vec4") color[4];
};
