#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F5History
{
    LUX_RESOURCE(role = push_constant, stages = 4) float camera {};
    LUX_RESOURCE(role = push_constant, stages = 4) float seconds {};
    LUX_RESOURCE(role = push_constant, stages = 4) unsigned int epoch {};
    LUX_RESOURCE(role = read_write_storage, stages = 4) lux::render::StorageBuffer<unsigned int> history;
    LUX_RESOURCE(role = storage_write, stages = 4, format = rgba32f) lux::render::StorageTexture output;
};
