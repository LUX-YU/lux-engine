#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    struct PipelineLayoutPlan;

    [[nodiscard]] RenderResult<void> planBarriers(
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        const PipelineLayoutPlan& layouts,
        std::span<CompiledResource> resources,
        RecordingProgram& recording,
        const PhysicalResourcePlan* physical = nullptr
    ) noexcept;
} // namespace lux::render::detail
