#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct alignas(16) LUX_PASS_SCALARS() F3Vector
{
    float x, y, z, w;
};

struct LUX_PASS_PARAMS() F3Storage
{
    LUX_RESOURCE(role = push_constant, stages = 4) unsigned int count { 4 };
    LUX_RESOURCE(role = uniform_read, scope = scene, stages = 4) lux::render::UniformBuffer<F3Vector> config;
    LUX_RESOURCE(role = read_only_storage, scope = feature, stages = 4) lux::render::StorageBuffer<F3Vector> input;
    LUX_RESOURCE(role = read_write_storage, stages = 4) lux::render::StorageBuffer<F3Vector> outputs[2];
    LUX_RESOURCE(role = storage_write, stages = 4, format = rgba32f) lux::render::StorageTexture image;
};
