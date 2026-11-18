#pragma once

#include <lux/engine/render/extensions/graph/Diagnostics.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{
    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpPlan(const PreparedRenderGraph&, EGraphDumpFormat, DumpSink) noexcept;
}
