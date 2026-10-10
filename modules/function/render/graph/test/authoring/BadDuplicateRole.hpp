#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() BadDuplicateRole
{
    LUX_RESOURCE(role = sampled_read, role = storage_write) lux::render::StorageTexture input;
};
