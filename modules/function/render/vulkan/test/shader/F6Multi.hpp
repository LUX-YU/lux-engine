#pragma once
#include "F6Light.hpp"
#include "F6Material.hpp"
#ifndef __LUX_PARSE_TIME__
#include "F6Light.data.hpp"
#include "F6Material.data.hpp"
#endif
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F6Multi
{
    LUX_RESOURCE(role = read_only_storage, scope = scene, stages = 4, semantic = "records")
    lux::render::StorageBuffer<LightGpu> main_light;
    LUX_RESOURCE(role = read_only_storage, scope = scene, stages = 4, semantic = "records")
    lux::render::StorageBuffer<LightGpu> preview_light;
    LUX_RESOURCE(role = read_only_storage, scope = scene, stages = 4, semantic = "records")
    lux::render::StorageBuffer<MaterialGpu> material;
    LUX_RESOURCE(role = read_write_storage, stages = 4) lux::render::StorageBuffer<float> output;
};
