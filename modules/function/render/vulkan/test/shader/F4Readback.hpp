#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F4Readback
{
    LUX_RESOURCE(role = transfer_source) lux::render::TransferTexture source;
    LUX_RESOURCE(role = transfer_destination) lux::render::TransferBuffer destination;
};
