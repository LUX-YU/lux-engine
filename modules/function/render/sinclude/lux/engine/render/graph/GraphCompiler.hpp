#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    class LUX_RENDER_PUBLIC GraphCompiler final
    {
    public:
        // Cold compilation consumes the caller's exact analysis. It never discovers business resources.
        [[nodiscard]] static RenderResult<RenderPlan> compile(
            nri::Device& device,
            const nri::CoreInterface& nri_core,
            std::array<const QueueProperties*, 3> queues,
            const RenderGraph& graph,
            const RenderGraphAnalysis& analysis,
            const EffectOutputDesc* target = nullptr,
            ETransientAllocation allocation = ETransientAllocation::ALIAS_MEMORY,
            std::span<const std::uint32_t> parameter_groups = {}
        ) noexcept;
    };
} // namespace lux::render::detail
