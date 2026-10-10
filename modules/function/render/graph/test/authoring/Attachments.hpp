#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() Attachments
{
    LUX_RESOURCE(role = color_attachment) lux::render::Attachment color;
    LUX_RESOURCE(role=resolve, for=color) lux::render::ResolveAttachment resolved;
    LUX_RESOURCE(role = depth_stencil) lux::render::DepthStencilAttachment depth;
};
