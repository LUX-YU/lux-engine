#pragma once

#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>

namespace lux::render::detail
{
    struct ResourceUseStages final
    {
        std::uint32_t resource_index;
        nri::StageBits stages;
    };

    // One invocation owns these intermediate tables, including across native preparation and queue-policy retries.
    struct GraphCompiler::Workspace final
    {
        AnalyzedGraph source;
        GraphDeviceFacts device;
        ETransientAllocation allocation;
        GraphQueuePolicy policy;
        ShaderBindingPlan bindings{};
        GraphCompileDiagnostics diagnostics{};
        std::vector<std::vector<ResourceUseStages>> use_stages{};
        std::vector<std::uint32_t> pass_batches{};

        [[nodiscard]] RenderResult<RenderGraphExecutable> planStructure(nri::Device*, const EffectOutputDesc*) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>>
        prepareOperations(PreparedRenderGraph&, execution::CpuScheduler) noexcept;
        [[nodiscard]] RenderResult<void> finishDevicePlan(PreparedRenderGraph&) noexcept;
        [[nodiscard]] RenderResult<void> finishPlan(RenderGraphExecutable&) noexcept;
        [[nodiscard]] RenderResult<void>
        planBarriers(RenderGraphExecutable&, const PhysicalResourcePlan* = nullptr) const noexcept;
        [[nodiscard]] PhysicalResourcePlan planPhysicalResources(const RenderGraphExecutable&) noexcept;
        [[nodiscard]] bool assignAsyncCompute(RecordingProgram&, std::uint32_t surface_output) const noexcept;
        [[nodiscard]] bool validRecordingTables(const RenderGraphExecutable&) const noexcept;
        void collectUseStages() noexcept;
        void planSubmissions(RenderGraphExecutable&) noexcept;
    };
}
