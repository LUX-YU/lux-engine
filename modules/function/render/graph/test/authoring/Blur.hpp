#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() Blur
{
    LUX_RESOURCE(role = sampled_read) lux::render::SampledTexture input[1];
 LUX_RESOURCE(role=sampler, for=input) lux::render::SamplerHandle linear;
 LUX_RESOURCE(role = color_attachment) lux::render::Attachment output;
 float radius{1.0f};
};
