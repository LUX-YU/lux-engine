#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() BadUnknown
{
    LUX_RESOURCE(role = sampled_read, requried = false) lux::render::SampledTexture input;
};
