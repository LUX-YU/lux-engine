#pragma once

#include <lux/engine/render/graph/compile/GraphAnalysis.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>
#include <optional>

namespace lux::render::detail
{
    struct EffectCompileRequest;
    struct EffectCompileEnvironment;
    struct GraphQueuePolicy final
    {
        GraphQueueCosts costs;
        // Native fixtures use this to exercise legal cross-queue transitions even on serialized graphs.
        bool exercise_async = false;
    };

    struct AnalyzedGraph final
    {
        const GraphDefinition& graph;
        const RenderGraphAnalysis& analysis;
        std::span<const GraphResourceId> concurrent_image_reads{};
    };

    struct GraphDeviceFacts final
    {
        nri::DeviceDesc limits;
        std::array<const QueueProperties*, 3> queues;
        bool device_addresses = false;
    };

    struct GraphCompileOptions final
    {
        std::optional<EffectOutputDesc> target;
        ETransientAllocation allocation = ETransientAllocation::ALIAS_MEMORY;
        bool exercise_async = false;
    };

    class LUX_RENDER_PUBLIC GraphCompiler final
    {
    public:
        using EffectResult = RenderResult<std::unique_ptr<const compiled::RenderGraph>>;
        [[nodiscard]] static exec::task<EffectResult>
        compileEffect(DeviceSession&, EffectCompileRequest, EffectCompileEnvironment) noexcept;

        // The graph and analysis live through task completion. Options are owned by the compile task.
        [[nodiscard]] static exec::task<RenderResult<PreparedRenderGraph>>
        compile(DeviceSession&, execution::CpuScheduler, AnalyzedGraph, GraphCompileOptions = {}) noexcept;

        [[nodiscard]] static RenderResult<compiled::RenderGraph> compileForRecordingTest(
            GraphDeviceFacts, AnalyzedGraph, ETransientAllocation = ETransientAllocation::INDEPENDENT,
            GraphQueuePolicy = {}
        ) noexcept;

    private:
        struct Workspace;
        [[nodiscard]] static RenderResult<void>
        lowerCommands(
            compiled::RenderGraph&, const GraphDefinition&, std::span<const GraphPassId>, const nri::DeviceDesc&
        ) noexcept;
    };
} // namespace lux::render::detail
