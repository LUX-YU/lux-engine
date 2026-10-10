#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_SCALARS() OptionalInputs
{
    LUX_RESOURCE(role = sampled_read, required = false, semantic = "optional.color")
    lux::render::SampledTexture input;
};

struct LUX_PASS_PARAMS() Optional
{
    OptionalInputs group;
    LUX_RESOURCE(role=sampler, for=group.input) lux::render::SamplerHandle linear;
    LUX_RESOURCE(role = color_attachment) lux::render::Attachment output;
};
