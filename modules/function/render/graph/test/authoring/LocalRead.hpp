#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() LocalRead
{
    LUX_RESOURCE(role = input_attachment, stages = 2) lux::render::SampledTexture input;
    LUX_RESOURCE(role = color_attachment) lux::render::Attachment output;
};
