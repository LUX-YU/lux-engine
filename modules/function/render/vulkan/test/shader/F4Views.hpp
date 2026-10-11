#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F4Views
{
    LUX_RESOURCE(role = push_constant, stages = 1) float view_index {};
    LUX_RESOURCE(role = push_constant, stages = 2) float first_value { 0.25f };
    LUX_RESOURCE(role = push_constant, stages = 2) float second_value { 0.75f };
    LUX_RESOURCE(role = color_attachment, stages = 2) lux::render::Attachment output;
};
