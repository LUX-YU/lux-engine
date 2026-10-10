#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F3Tonemap
{
    LUX_RESOURCE(role = push_constant, stages = 2) float exposure { 1.0f };
    LUX_RESOURCE(role = sampled_read, stages = 2) lux::render::SampledTexture input;
    LUX_RESOURCE(role = sampler, stages = 2, for = input) lux::render::SamplerHandle nearest;
    LUX_RESOURCE(role = color_attachment, stages = 2) lux::render::Attachment output;
};
