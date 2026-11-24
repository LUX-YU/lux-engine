#pragma once

#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/graph/compile/GraphAnalysis.hpp>
#include <lux/engine/render/effect/EffectDeviceGraph.hpp>
#include <lux/engine/render/device/DeviceOptions.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <atomic>
#include <optional>

namespace lux::render { class ResourceRegistry; struct EffectBuildDevice; }
namespace lux::render::runtime_detail { struct EffectDeclarations; }
namespace lux::render::effect_detail { struct ValidatedEffectGraph; }

namespace lux::render::detail
{
    struct GraphCompileDiagnostics;

    struct EffectCompileConfig final
    {
        GraphOutputRequirements output;
        const ResourceRegistry& resource_types;
        bool uploads_available;
        std::optional<EGraphUse> target_final_use;
    };

    struct EffectCompileEnvironment final
    {
        execution::CpuScheduler cpu;
        RenderErrorSink diagnostics;
        const std::atomic<bool>& stopping;
        const RenderError& device_failure;
    };

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
        std::optional<GraphOutputRequirements> target;
        ETransientAllocation allocation = ETransientAllocation::ALIAS_MEMORY;
        bool exercise_async = false;
        // Optional caller-owned destination, replaced only when native compilation succeeds.
        GraphCompileDiagnostics* diagnostics = nullptr;
    };

    class LUX_RENDER_PUBLIC RenderGraphCompiler final
    {
    public:
        RenderGraphCompiler(DeviceSession& session, EffectCompileEnvironment environment) noexcept
            : session_(session), environment_(environment) {}
        RenderGraphCompiler(const RenderGraphCompiler&) = delete;
        RenderGraphCompiler& operator=(const RenderGraphCompiler&) = delete;

        using EffectResult = RenderResult<EffectDeviceGraph>;
        // The owning Effect and the compiler services remain available until the task rejoins.
        [[nodiscard]] exec::task<EffectResult>
        compile(const effect_detail::ValidatedEffectGraph&, EffectCompileConfig) & noexcept;

        // The graph and analysis live through task completion. Options are owned by the compile task.
        [[nodiscard]] static exec::task<RenderResult<compiled::RenderGraph>>
        compileNative(DeviceSession&, execution::CpuScheduler, AnalyzedGraph, GraphCompileOptions = {}) noexcept;

        [[nodiscard]] static RenderResult<compiled::RenderGraph> compileForRecordingTest(
            GraphDeviceFacts, AnalyzedGraph, ETransientAllocation = ETransientAllocation::INDEPENDENT,
            GraphQueuePolicy = {}
        ) noexcept;

    private:
        DeviceSession& session_;
        EffectCompileEnvironment environment_;

        struct Workspace;
        [[nodiscard]] RenderResult<GraphDefinition> lowerEffect(
            const effect_detail::ValidatedEffectGraph&, const EffectCompileConfig&,
            const EffectBuildDevice&, runtime_detail::EffectDeclarations&
        ) const noexcept;
        [[nodiscard]] static RenderResult<void>
        lowerCommands(
            compiled::RenderGraph&, const GraphDefinition&, std::span<const GraphPassId>, const nri::DeviceDesc&
        ) noexcept;
    };
} // namespace lux::render::detail
