#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F4CArguments
{
    LUX_RESOURCE(role = read_write_storage, stages = 4) lux::render::StorageBuffer<unsigned int> output;
    LUX_RESOURCE(role = push_constant, stages = 4) unsigned int indexed {};
};
