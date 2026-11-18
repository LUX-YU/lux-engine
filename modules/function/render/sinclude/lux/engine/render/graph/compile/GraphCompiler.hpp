#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    class LUX_RENDER_PUBLIC GraphCompiler final
    {
    public:
        // Sole device compilation entry. Returns a fully prepared immutable native candidate.
        [[nodiscard]] static exec::task<RenderResult<RenderPlan>> compile(
            DeviceSession&, execution::CpuScheduler, const RenderGraph&, const RenderGraphAnalysis&,
            const EffectOutputDesc* target = nullptr,
            ETransientAllocation allocation = ETransientAllocation::ALIAS_MEMORY,
            std::span<const std::uint32_t> parameter_groups = {}
        ) noexcept;

    private:
        [[nodiscard]] static RenderResult<RenderPlan> planStructure(
            nri::Device&, const nri::CoreInterface&, std::array<const QueueProperties*, 3>, const RenderGraph&,
            const RenderGraphAnalysis&, const EffectOutputDesc*, ETransientAllocation, std::span<const std::uint32_t>,
            bool
        ) noexcept;
        [[nodiscard]] static RenderResult<void> finishPlan(
            RenderPlan&, const RenderGraph&, const RenderGraphAnalysis&, std::array<const QueueProperties*, 3>,
            const nri::DeviceDesc&, ETransientAllocation
        ) noexcept;
    };
} // namespace lux::render::detail
