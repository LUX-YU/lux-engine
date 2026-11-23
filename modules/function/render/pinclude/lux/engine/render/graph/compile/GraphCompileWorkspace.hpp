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

    struct AliasResourceUse final
    {
        GraphResourceId resource;
        AliasUsePoint point;
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
        std::vector<AliasResourceUse> alias_uses{};

        [[nodiscard]] bool presentsOutput() const noexcept;
        [[nodiscard]] RenderResult<compiled::RenderGraph> planStructure(const GraphOutputRequirements*) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>>
        prepareOperations(DeviceSession&, PreparedRenderGraph&, execution::CpuScheduler) noexcept;
        static void compileFixedBindings(compiled::RenderGraph&) noexcept;
        [[nodiscard]] RenderResult<void> finishDevicePlan(PreparedRenderGraph&) noexcept;
        [[nodiscard]] RenderResult<void> finishPlan(compiled::RenderGraph&) noexcept;
        [[nodiscard]] RenderResult<void>
        planBarriers(compiled::RenderGraph&, const PhysicalResourcePlan* = nullptr) const noexcept;
        [[nodiscard]] PhysicalResourcePlan planPhysicalResources(const compiled::RenderGraph&) noexcept;
        [[nodiscard]] bool assignAsyncCompute(compiled::RenderGraph&, std::uint32_t surface_output) const noexcept;
        [[nodiscard]] bool validRecordingTables(const compiled::RenderGraph&) const noexcept;
        [[nodiscard]] RecordingDependencies recordingDependencies(const compiled::RenderGraph&) const noexcept;
        void publishAliasFrontiers(compiled::RenderGraph&) const noexcept;
        void collectUseStages() noexcept;
        void planSubmissions(compiled::RenderGraph&) noexcept;
    };
}
