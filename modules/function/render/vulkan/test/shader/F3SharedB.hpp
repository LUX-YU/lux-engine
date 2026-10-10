#pragma once
#include "F3SharedValue.hpp"
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F3SharedB
{
    LUX_RESOURCE(role = uniform_read, scope = scene, stages = 1) lux::render::UniformBuffer<F3SharedValue> scene_b;
    LUX_RESOURCE(role = uniform_read, scope = feature, stages = 2) lux::render::UniformBuffer<F3SharedValue> feature_b;
    LUX_RESOURCE(role = color_attachment, stages = 2) lux::render::Attachment output;
};
