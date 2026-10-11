#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F4Raster
{
    LUX_RESOURCE(role = push_constant, stages = 2) float red {};
    LUX_RESOURCE(role = push_constant, stages = 2) float green {};
    LUX_RESOURCE(role = push_constant, stages = 2) float blue {};
    LUX_RESOURCE(role = push_constant, stages = 2) float alpha { 1 };
    LUX_RESOURCE(role = push_constant, stages = 1) float depth_value { 0.5f };
    LUX_RESOURCE(role = color_attachment, stages = 2) lux::render::Attachment output;
    LUX_RESOURCE(role = resolve, for = output, stages = 2) lux::render::ResolveAttachment resolved;
    LUX_RESOURCE(role = depth_stencil, stages = 2) lux::render::DepthStencilAttachment depth;
};
