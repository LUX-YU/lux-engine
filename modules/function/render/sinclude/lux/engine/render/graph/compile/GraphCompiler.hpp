#pragma once

#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>

namespace lux::render::detail
{
    struct ShaderBindingPlan;

    struct GraphQueuePolicy final
    {
        GraphQueueCosts costs;
        // Native fixtures use this to exercise legal cross-queue transitions even on serialized graphs.
        bool exercise_async = false;
    };

    class LUX_RENDER_PUBLIC GraphCompiler final
    {
    public:
        // Sole device compilation entry. Returns a fully prepared immutable native candidate.
        [[nodiscard]] static exec::task<RenderResult<PreparedRenderGraph>> compile(
            DeviceSession&, execution::CpuScheduler, const RenderGraph&, const RenderGraphAnalysis&,
            const EffectOutputDesc* target = nullptr,
            ETransientAllocation allocation = ETransientAllocation::ALIAS_MEMORY,
            std::span<const std::uint32_t> parameter_groups = {},
            bool exercise_async = false
        ) noexcept;

        // Use native-independent compilation to exercise recording tables with declared capabilities.
        [[nodiscard]] static RenderResult<RenderGraphExecutable> compileForRecordingTest(
            const nri::DeviceDesc&, std::array<const QueueProperties*, 3>, const RenderGraph&,
            const RenderGraphAnalysis&, ETransientAllocation = ETransientAllocation::INDEPENDENT,
            GraphQueuePolicy = {}
        ) noexcept;

    private:
        [[nodiscard]] static RenderResult<void>
        lowerCommands(RenderGraphExecutable&, const RenderGraph&, std::span<const GraphPassId>,
                      const nri::DeviceDesc&) noexcept;
        [[nodiscard]] static exec::task<RenderResult<void>>
        prepareOperations(
            const RenderGraphExecutable&, ShaderBindingPlan&, DeviceSession&, NativeGraphPrograms&, const RenderGraph&,
            std::span<const GraphPassId>, execution::CpuScheduler
        ) noexcept;
        [[nodiscard]] static RenderResult<RenderGraphExecutable> planStructure(
            nri::Device*, const nri::DeviceDesc&, std::array<const QueueProperties*, 3>, const RenderGraph&,
            const RenderGraphAnalysis&, const EffectOutputDesc*, ETransientAllocation, std::span<const std::uint32_t>,
            bool, ShaderBindingPlan&
        ) noexcept;
        [[nodiscard]] static RenderResult<void> finishPlan(
            RenderGraphExecutable&, const RenderGraph&, const RenderGraphAnalysis&, std::array<const QueueProperties*, 3>,
            const nri::DeviceDesc&, ETransientAllocation, GraphCompileDiagnostics&, GraphQueuePolicy,
            ShaderBindingPlan&, GraphAssemblyPlan&
        ) noexcept;
    };
} // namespace lux::render::detail
