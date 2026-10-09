#pragma once

#include <lux/engine/description/Script.hpp>
#include <lux/engine/flowforge/FlowForgeFailure.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/flowforge/visibility.h>
#include <lux/engine/function/graph/GraphTypes.hpp>

#include <memory>
#include <span>

namespace lux::flowforge
{
    class FlowGraph;

    struct FlowAnalysisOptions final
    {
        ScriptAbilityNodeCatalogView script_abilities;
        std::span<const script::ScriptEventSourceDescription> script_events;
        rdesc::ScriptLifecycleRoles lifecycle;
    };

    // Disposable compilation facts for one immutable graph input. Owns requirements and execution
    // reachability; it neither owns nor borrows nodes, pins, reflection metadata or the input catalogs.
    // Editing the graph requires a new analysis. This is not an authoring store or a runtime executor.
    class LUX_ENGINE_FLOWFORGE_PUBLIC FlowAnalysis final
    {
    public:
        [[nodiscard]] static FlowForgeResult<FlowAnalysis> create(
            const FlowGraph&,
            const FlowAnalysisOptions& = {}
        ) noexcept;

        ~FlowAnalysis();
        FlowAnalysis(FlowAnalysis&&) noexcept;
        FlowAnalysis& operator=(FlowAnalysis&&) noexcept;
        FlowAnalysis(const FlowAnalysis&) = delete;
        FlowAnalysis& operator=(const FlowAnalysis&) = delete;

        [[nodiscard]] std::span<const rdesc::ScriptApiRequirement> abilityRequirements() const noexcept;
        [[nodiscard]] std::span<const script::ScriptEventSourceDescription> eventRequirements() const noexcept;

        // IDs refer to the analyzed input. Invalid/unconnected start pins have no witness. The
        // smallest reachable suspension NodeId is chosen deterministically, including recursive calls.
        [[nodiscard]] graph::NodeId firstSuspensionFrom(graph::PinId start) const noexcept;
        [[nodiscard]] graph::NodeId suspensionBetween(graph::PinId start, graph::NodeId target) const noexcept;

    private:
        struct Impl;
        explicit FlowAnalysis(std::unique_ptr<Impl>) noexcept;

        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::flowforge
