#pragma once

#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{
    class LUX_RENDER_PUBLIC GraphCompiler final
    {
    public:
        // Sole device compilation entry. Returns a fully prepared immutable native candidate.
        [[nodiscard]] static exec::task<RenderResult<PreparedRenderGraph>> compile(
            DeviceSession&, execution::CpuScheduler, const RenderGraph&, const RenderGraphAnalysis&,
            const EffectOutputDesc* target = nullptr,
            ETransientAllocation allocation = ETransientAllocation::ALIAS_MEMORY,
            std::span<const std::uint32_t> parameter_groups = {}
        ) noexcept;

        // Use native-independent compilation to exercise recording tables with declared capabilities.
        [[nodiscard]] static RenderResult<RenderGraphExecutable> compileForRecordingTest(
            const nri::DeviceDesc&, std::array<const QueueProperties*, 3>, const RenderGraph&,
            const RenderGraphAnalysis&, ETransientAllocation = ETransientAllocation::INDEPENDENT
        ) noexcept;

    private:
        [[nodiscard]] static RenderResult<RenderGraphExecutable> planStructure(
            nri::Device*, const nri::DeviceDesc&, std::array<const QueueProperties*, 3>, const RenderGraph&,
            const RenderGraphAnalysis&, const EffectOutputDesc*, ETransientAllocation, std::span<const std::uint32_t>,
            bool, std::vector<std::uint32_t>&
        ) noexcept;
        [[nodiscard]] static RenderResult<void> finishPlan(
            RenderGraphExecutable&, const RenderGraph&, const RenderGraphAnalysis&, std::array<const QueueProperties*, 3>,
            const nri::DeviceDesc&, ETransientAllocation
        ) noexcept;
    };
} // namespace lux::render::detail
