#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Authoring.hpp>

struct LUX_PASS_PARAMS() F5CReadBuffer
{
    LUX_RESOURCE(role = transfer_source) lux::render::TransferBuffer source;
};
