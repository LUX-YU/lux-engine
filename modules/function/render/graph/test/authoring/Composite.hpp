#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() Composite
{
    LUX_RESOURCE(role = sampled_read) lux::render::SampledTexture inputs[2];
 LUX_RESOURCE(role=sampler, for=inputs) lux::render::SamplerHandle linear;
 LUX_RESOURCE(role = color_attachment) lux::render::Attachment output;
};
