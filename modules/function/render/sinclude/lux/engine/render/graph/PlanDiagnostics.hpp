#pragma once

#include <lux/engine/render/graph/Diagnostics.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::detail
{
    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpPlan(const PreparedRenderGraph&, EGraphDumpFormat, DumpSink) noexcept;
}
