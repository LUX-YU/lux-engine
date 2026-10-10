#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() Hzb
{
    LUX_RESOURCE(role = sampled_read) lux::render::SampledTexture source;
 LUX_RESOURCE(role=sampler, for=source) lux::render::SamplerHandle nearest;
 LUX_RESOURCE(role = storage_write, format = r32f) lux::render::StorageTexture destination;
};
