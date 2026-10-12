#pragma once
#include <lux/engine/render/data/Annotations.hpp>

struct LUX_RENDER_DATA(
    name = "lux.test.f6.material",
    version = 1,
    scope = "scene",
    authority = "cpu",
    gpu = "std430"
) MaterialGpu
{
    float LUX_GPU_FIELD(shape = "vec4") tint[4];
    float roughness;
};
