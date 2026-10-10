#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct alignas(16) LUX_PASS_SCALARS() AlignedValue
{
    float x, y, z, w;
};

struct LUX_PASS_SCALARS() ComplexValue
{
    float gain;
    float weights[3];
};

struct LUX_PASS_PARAMS() Complex
{
    LUX_RESOURCE(role = uniform_read, scope = feature) lux::render::UniformBuffer<AlignedValue> block;
    LUX_RESOURCE(role = read_only_storage) lux::render::StorageBuffer<ComplexValue> values;
    LUX_RESOURCE(role = storage_write, format = rgba32f) lux::render::StorageTexture output;
    ComplexValue settings[2];
};
