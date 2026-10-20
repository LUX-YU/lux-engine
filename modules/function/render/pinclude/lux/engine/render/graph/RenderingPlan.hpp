#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    [[nodiscard]] RenderResult<void> planRendering(
        const RenderGraphAnalysis& analysis,
        std::span<CompiledResource> resources,
        RecordingProgram& recording
    ) noexcept;
}
