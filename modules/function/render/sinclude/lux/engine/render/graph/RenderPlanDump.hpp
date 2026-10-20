#pragma once

#include <lux/engine/render/graph/GraphDump.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpRenderPlan(const RenderPlan&, EGraphDumpFormat, GraphDumpSink) noexcept;
}
