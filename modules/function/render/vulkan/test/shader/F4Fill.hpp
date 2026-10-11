#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F4Fill
{
    LUX_RESOURCE(role = storage_write, stages = 4, format = rgba32f) lux::render::StorageTexture output;
};
