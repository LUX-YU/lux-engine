#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_SCALARS() HalfStorageElement
{
    float value;
};

struct LUX_PASS_PARAMS() HalfStorage
{
    LUX_RESOURCE(role = read_write_storage, scope = scene) lux::render::StorageBuffer<HalfStorageElement> values;
    LUX_RESOURCE(role = storage_write, format = rgba16f) lux::render::StorageTexture output;
};
