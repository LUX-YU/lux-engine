#pragma once

#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FlowSourceData.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/function/script/ScriptEvent.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <optional>
#include <variant>

namespace lux::flowforge
{
    class FlowNodeCatalog;

    // Cold metadata remains immutable. A dynamic contribution supplies an owner for every referenced
    // span, descriptor and module; static program-lifetime metadata needs no lease. A materialized graph
    // still borrows this environment, so its owner must retain the environment until graph destruction.
    struct FlowSourceEnvironment final
    {
        std::span<const lux::meta::RefType* const> types;
        std::span<const lux::meta::RefClass* const> classes;
        std::span<const lux::meta::RefFunction* const> functions;
        ScriptAbilityNodeCatalogView abilities;
        std::span<const lux::script::ScriptEventSourceDescription> events;
        std::shared_ptr<const void> code_lifetime;
        // Borrowed for this synchronous materialization only. Each restored node retains its
        // immutable definition and code lease; the catalog itself need not outlive the graph.
        const FlowNodeCatalog* nodes{};
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowSourceEnvironment(
        const FlowSourceEnvironment&,
        FlowSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowSource(
        const FlowSource&,
        FlowSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<std::string> encodeFlowSource(
        const FlowSource&,
        FlowSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSource> decodeFlowSource(
        std::string_view,
        FlowSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSource> captureFlowSource(
        lux::asset::AssetId,
        std::string name,
        const FlowGraph&,
        FlowSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSourceNode> captureFlowNode(
        const FlowGraph&,
        NodeId
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<std::vector<FuncArgInfo>> materializeFlowArguments(
        std::span<const FlowSourceArgument>,
        const FlowSourceEnvironment& = {},
        FlowSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSourceLiteral>
    captureFlowLiteral(const lux::meta::RuntimeObject&) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<lux::meta::RuntimeObject>
    materializeFlowLiteral(const FlowSourceLiteral&, const lux::meta::RefType&) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowVariable(
        const FlowSourceVariable&,
        FlowSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSourceVariable>
    captureFlowVariable(const FlowGraph::GraphVariable&) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowGraph::GraphVariable> materializeFlowVariable(
        const FlowSourceVariable&,
        const FlowSourceEnvironment& = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowGraph> materializeFlowSource(
        const FlowSource&,
        const FlowSourceEnvironment& = {},
        FlowSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowSourceConnection(
        const FlowSourcePin& from,
        const FlowSourcePin& to,
        const FlowSourceEnvironment& = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowSourceLiteral(
        const FlowSourcePin&,
        const FlowSourceEnvironment& = {}
    ) noexcept;
} // namespace lux::flowforge
