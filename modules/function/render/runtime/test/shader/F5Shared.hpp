#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct alignas(16) LUX_PASS_SCALARS() F5Color
{
    float red, green, blue, alpha;
};

struct LUX_PASS_PARAMS() F5Shared
{
    LUX_RESOURCE(role = push_constant, stages = 4) float factor { 1 };
    LUX_RESOURCE(role = uniform_read, stages = 4) lux::render::UniformBuffer<F5Color> asset;
    LUX_RESOURCE(role = storage_write, stages = 4, format = rgba32f) lux::render::StorageTexture output;
};
