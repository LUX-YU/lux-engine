#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F4Hzb
{
    LUX_RESOURCE(role = sampled_read, stages = 4) lux::render::SampledTexture source;
    LUX_RESOURCE(role = sampler, for = source, stages = 4) lux::render::SamplerHandle nearest;
    LUX_RESOURCE(role = storage_write, stages = 4, format = rgba32f) lux::render::StorageTexture destination;
};
