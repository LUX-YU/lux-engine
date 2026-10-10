#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() NonShader
{
    LUX_RESOURCE(role = color_attachment) lux::render::Attachment color;
    LUX_RESOURCE(role=resolve, for=color) lux::render::ResolveAttachment resolved;
    LUX_RESOURCE(role = depth_stencil) lux::render::DepthStencilAttachment depth;
    LUX_RESOURCE(role = transfer_source) lux::render::TransferBuffer source;
    LUX_RESOURCE(role = transfer_destination) lux::render::TransferBuffer destination;
    LUX_RESOURCE(role = vertex) lux::render::VertexBuffer vertices;
    LUX_RESOURCE(role = index) lux::render::IndexBuffer indices;
    LUX_RESOURCE(role = indirect) lux::render::IndirectBuffer indirect;
};
