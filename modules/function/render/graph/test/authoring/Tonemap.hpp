#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() Tonemap
{
    LUX_RESOURCE(role = sampled_read) lux::render::SampledTexture input;
 LUX_RESOURCE(role=sampler, for=input) lux::render::SamplerHandle linear;
 LUX_RESOURCE(role = color_attachment) lux::render::Attachment output;
 LUX_RESOURCE(role = push_constant, frequency = draw, stages = 2) float exposure { 1.0f };
};
