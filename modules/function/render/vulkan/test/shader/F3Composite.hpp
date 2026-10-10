#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F3Composite
{
    LUX_RESOURCE(role = push_constant, stages = 2) float weight { 0.25f };
    LUX_RESOURCE(role = sampled_read, stages = 2) lux::render::SampledTexture inputs[2];
    LUX_RESOURCE(role = sampler, stages = 2, for = inputs) lux::render::SamplerHandle nearest;
    LUX_RESOURCE(role = color_attachment, stages = 2) lux::render::Attachment output;
};
