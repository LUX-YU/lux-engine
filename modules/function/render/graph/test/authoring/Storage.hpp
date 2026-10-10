#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_SCALARS() StorageElement
{
    float value;
};

struct LUX_PASS_PARAMS() Storage
{
    LUX_RESOURCE(role = read_write_storage, scope = scene) lux::render::StorageBuffer<StorageElement> values;
    LUX_RESOURCE(role = storage_write, format = rgba32f) lux::render::StorageTexture output;
};
