#pragma once

#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::detail
{
    [[nodiscard]] RenderResult<void> planRendering(
        const RenderGraphAnalysis& analysis,
        std::span<CompiledResource> resources,
        RecordingProgram& recording
    ) noexcept;
}
