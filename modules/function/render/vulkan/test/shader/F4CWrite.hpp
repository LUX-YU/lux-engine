#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F4CWrite
{
    LUX_RESOURCE(role = read_write_storage, stages = 4) lux::render::StorageBuffer<unsigned int> output;
};
