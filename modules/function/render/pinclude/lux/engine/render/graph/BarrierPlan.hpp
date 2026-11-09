#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    struct ShaderBindingPlan;

    [[nodiscard]] std::vector<std::vector<std::pair<std::uint32_t, nri::StageBits>>>
    collectUseStages(const RenderGraph&, const ShaderBindingPlan&) noexcept;

    // Compact only fixed subresource ranges; publish reverse indexes after all planning mutations.
    void compactImageTransitions(std::span<CompiledResource>, RecordingProgram&) noexcept;

    [[nodiscard]] RenderResult<void> planBarriers(
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        const ShaderBindingPlan& layouts,
        std::span<CompiledResource> resources,
        RecordingProgram& recording,
        const EffectOutputDesc& target,
        const PhysicalResourcePlan* physical = nullptr
    ) noexcept;
} // namespace lux::render::detail
