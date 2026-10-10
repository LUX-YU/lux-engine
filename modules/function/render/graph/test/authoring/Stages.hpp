#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct alignas(16) LUX_PASS_SCALARS() StageVector
{
    float x, y, z, w;
};

struct alignas(16) LUX_PASS_SCALARS() StageUniform
{
    StageVector rows[4];
};

struct LUX_PASS_PARAMS() Stages
{
    LUX_RESOURCE(role = uniform_read, stages = 1) lux::render::UniformBuffer<StageUniform> vertex;
    LUX_RESOURCE(role = uniform_read, stages = 3) lux::render::UniformBuffer<StageUniform> shared;
    LUX_RESOURCE(role = read_only_storage, stages = 2) lux::render::StorageBuffer<StageVector> fragment;
    LUX_RESOURCE(role = sampled_read, stages = 2, dimension = 2DArray) lux::render::SampledTexture image;
    LUX_RESOURCE(role = sampler, stages = 2, for = image) lux::render::SamplerHandle linear;
    LUX_RESOURCE(role = push_constant, stages = 1) float vertex_scale;
    LUX_RESOURCE(role = push_constant, stages = 2) float fragment_scale;
    LUX_RESOURCE(role = push_constant, stages = 3) float common_scale;
};
