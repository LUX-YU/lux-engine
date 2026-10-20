#pragma once

#include <lux/engine/render/graph/Diagnostics.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpPlan(const RenderPlan&, EGraphDumpFormat, DumpSink) noexcept;
}
