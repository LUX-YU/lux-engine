#include <lux/engine/toolchain/flowforge/ScriptCompiler.hpp>

#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/simulation/ScriptBindingCompatibility.hpp>

#include <algorithm>

namespace lux::flowforge
{
    namespace
    {
        [[nodiscard]] lux::rdesc::ScriptValueType copyType(
            const lux::script::ScriptSemanticType& type
        )
        {
            return lux::rdesc::ScriptValueType{
                std::string(type.canonical_name),
                type.type_id,
                type.pass};
        }
    }

    lux::cxx::expected<FlowForgeScriptArtifact, EFlowForgeCompileError>
    compileFlowForgeScript(
        std::string module_name,
        lux::rdesc::EScriptModel model,
        std::span<const ExportMethodNode> graph_exports,
        std::span<const BindingEdge> graph_bindings,
        const lux::simulation::SimulationDescription& simulation,
        FlowForgeStateLayout state
    )
    {
        if (module_name.empty())
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_MODULE_NAME);
        }
        if (state.defaults.size() > state.size || state.align == 0U ||
            (state.align & (state.align - 1U)) != 0U)
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_STATE_LAYOUT);
        }
        if (!validFlowForgeGraph(graph_exports, graph_bindings))
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_GRAPH);
        }

        FlowForgeScriptArtifact result;
        result.description.schema_version = lux::rdesc::Script::kSchemaVersion;
        result.description.module_name = std::move(module_name);
        result.description.model = model;
        result.description.body = lux::rdesc::NativeModuleScript{
            LUX_SCRIPT_ABI_VERSION,
            state.hash,
            state.size,
            state.align,
            state.defaults};
        result.abi.abi_version = LUX_SCRIPT_ABI_VERSION;
        result.abi.state = std::move(state);

        for (const auto& node : graph_exports)
        {
            const auto symbol = lux::script::scriptSymbolId(
                result.description.module_name,
                node.name,
                {node.parameters, node.returns}
            );
            if (symbol == lux::script::InvalidScriptSymbolId ||
                std::find(
                    result.abi.symbols.begin(),
                    result.abi.symbols.end(),
                    symbol) != result.abi.symbols.end())
            {
                return lux::cxx::unexpected(
                    EFlowForgeCompileError::DUPLICATE_SYMBOL);
            }
            lux::rdesc::ScriptFunction function;
            function.name = node.name;
            function.symbol_id = symbol;
            function.args.reserve(node.parameters.size());
            function.returns.reserve(node.returns.size());
            for (const auto& parameter : node.parameters)
                function.args.push_back(copyType(parameter));
            for (const auto& return_type : node.returns)
                function.returns.push_back(copyType(return_type));
            result.description.exports.push_back(std::move(function));
            result.abi.symbols.push_back(symbol);
        }

        for (const auto& edge : graph_bindings)
        {
            const auto node = std::find_if(
                graph_exports.begin(),
                graph_exports.end(),
                [&](const ExportMethodNode& value) noexcept
                {
                    return value.id == edge.export_node;
                }
            );
            const auto function = std::find_if(
                result.description.exports.begin(),
                result.description.exports.end(),
                [&](const lux::rdesc::ScriptFunction& value) noexcept
                {
                    return value.name == node->name;
                }
            );
            if (lux::simulation::evaluateScriptBindingCompatibility(
                    simulation,
                    model,
                    *function,
                    edge.target) !=
                lux::simulation::EScriptBindingCompatibility::COMPATIBLE)
            {
                return lux::cxx::unexpected(
                    EFlowForgeCompileError::INCOMPATIBLE_BINDING);
            }
            result.binding_template.push_back({
                function->symbol_id,
                edge.target});
        }

        if (!lux::rdesc::validScriptDescription(result.description))
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_DESCRIPTION);
        }
        return result;
    }
}
