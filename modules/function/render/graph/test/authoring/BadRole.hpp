#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() BadRole
{
    LUX_RESOURCE(role = sampled_read) lux::render::TransferBuffer input;
};
