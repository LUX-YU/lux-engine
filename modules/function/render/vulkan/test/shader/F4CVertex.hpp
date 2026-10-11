#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F4CVertex
{
    LUX_RESOURCE(role = vertex) lux::render::VertexBuffer vertices;
    LUX_RESOURCE(role = color_attachment, stages = 2) lux::render::Attachment output;
};
