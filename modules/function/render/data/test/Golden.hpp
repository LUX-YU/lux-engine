#pragma once
#include <array>
#include <lux/engine/render/data/Annotations.hpp>
#include <string>
#include <vector>

struct LUX_DATA_VALUE() Position
{
    float LUX_GPU_FIELD(shape = "vec3") xyz[3];
    float radius;
};

struct LUX_RENDER_DATA(
    name = "lux.test.light",
    version = 1,
    scope = "scene",
    authority = "cpu",
    gpu = "std430"
) LightGpu
{
    Position position;
    float LUX_GPU_FIELD(shape = "vec4") color[4];
};

struct LUX_RENDER_DATA(
    name = "lux.test.sprite",
    version = 1,
    scope = "view",
    authority = "cpu",
    gpu = "std140"
) SpriteGpu
{
    float LUX_GPU_FIELD(shape = "vec2") position[2];
    float opacity;
    float weights[3];
};

struct LUX_RENDER_DATA(name = "lux.test.cpu", version = 1, scope = "runtime", authority = "cpu") CpuOnly
{
    std::string name;
    std::vector<int> values;
};

struct LUX_RENDER_DATA(
    name = "lux.test.wire",
    version = 2,
    scope = "runtime",
    authority = "cpu",
    cpu = false,
    wire = "lux.json",
    wire_version = 3
) LUX_META(serializable) WireOnly
{
    unsigned int LUX_META(serializable, required) sequence;
    std::string LUX_META(serializable, required) message;
};

struct LUX_RENDER_DATA(
    name = "lux.test.complex",
    version = 1,
    scope = "scene",
    authority = "cpu",
    gpu = "std140"
) ComplexGpu
{
    Position positions[2];
    float LUX_GPU_FIELD(shape = "mat3") basis[3][3];
    std::array<float, 2> gains;
};

struct LUX_RENDER_DATA(
    name = "lux.test.particle",
    version = 1,
    scope = "scene",
    authority = "gpu",
    gpu = "std430"
) ParticleGpu
{
    float LUX_GPU_FIELD(shape = "vec4") position[4];
    float age;
};

struct LUX_RENDER_DATA(
    name = "lux.test.integer",
    version = 1,
    scope = "scene",
    authority = "cpu",
    gpu = "std430"
) IntegerGpu
{
    int signed_value;
    unsigned int unsigned_value;
    int LUX_GPU_FIELD(shape = "ivec2") pair[2];
};
