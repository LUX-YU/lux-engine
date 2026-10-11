#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F4CIndexedIndirect
{
    LUX_RESOURCE(role = vertex) lux::render::VertexBuffer vertices;
    LUX_RESOURCE(role = index) lux::render::IndexBuffer indices;
    LUX_RESOURCE(role = indirect) lux::render::IndirectBuffer arguments;
    LUX_RESOURCE(role = color_attachment, stages = 2) lux::render::Attachment output;
};
