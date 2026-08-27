#include <lux/engine/toolchain/flowforge/ScriptCompiler.hpp>

#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/simulation/ScriptBindingCompatibility.hpp>

#include <algorithm>

namespace lux::flowforge
{
    lux::cxx::expected<FlowForgeScriptArtifact, EFlowForgeCompileError> compileFlowForgeScript(
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
            return lux::cxx::unexpected(EFlowForgeCompileError::INVALID_MODULE_NAME);
        }
        if (state.defaults.size() > state.size || state.align == 0U || (state.align & (state.align - 1U)) != 0U)
        {
            return lux::cxx::unexpected(EFlowForgeCompileError::INVALID_STATE_LAYOUT);
        }
        if (!validFlowForgeGraph(graph_exports, graph_bindings))
        {
            return lux::cxx::unexpected(EFlowForgeCompileError::INVALID_GRAPH);
        }

        FlowForgeScriptArtifact result;
        result.description.schema_version = lux::rdesc::Script::kSchemaVersion;
        result.description.module_name = std::move(module_name);
        result.description.model = model;
        result.description.body =
            lux::rdesc::NativeModuleScript{LUX_SCRIPT_ABI_VERSION, state.hash, state.size, state.align, state.defaults};
        result.abi.abi_version = LUX_SCRIPT_ABI_VERSION;
        result.abi.state = std::move(state);

        struct GeneratedExport final
        {
            FlowForgeExportNodeId node;
            std::size_t index{};
        };
        std::vector<GeneratedExport> generated_exports;
        generated_exports.reserve(graph_exports.size());
        for (const auto& node : graph_exports)
        {
            std::vector<lux::script::ScriptSemanticType> parameter_views;
            std::vector<lux::script::ScriptSemanticType> return_views;
            parameter_views.reserve(node.parameters.size());
            return_views.reserve(node.returns.size());
            for (const auto& parameter : node.parameters)
            {
                parameter_views.push_back({parameter.type_id, parameter.canonical_name, parameter.pass});
            }
            for (const auto& return_type : node.returns)
            {
                return_views.push_back({return_type.type_id, return_type.canonical_name, return_type.pass});
            }
            const auto symbol =
                lux::script::scriptSymbolId(result.description.module_name, node.name, {parameter_views, return_views});
            if (symbol == lux::script::InvalidScriptSymbolId ||
                std::find(result.abi.symbols.begin(), result.abi.symbols.end(), symbol) != result.abi.symbols.end())
            {
                return lux::cxx::unexpected(EFlowForgeCompileError::DUPLICATE_SYMBOL);
            }
            lux::rdesc::ScriptFunction function;
            function.name = node.name;
            function.symbol_id = symbol;
            function.args = node.parameters;
            function.returns = node.returns;
            generated_exports.push_back({node.id, result.description.exports.size()});
            result.description.exports.push_back(std::move(function));
            result.abi.symbols.push_back(symbol);
        }

        for (const auto& edge : graph_bindings)
        {
            const auto generated = std::find_if(
                generated_exports.begin(),
                generated_exports.end(),
                [&](const GeneratedExport& value) noexcept { return value.node == edge.export_node; }
            );
            const auto& function = result.description.exports[generated->index];
            if (lux::simulation::evaluateScriptBindingCompatibility(simulation, model, function, edge.target) !=
                lux::simulation::EScriptBindingCompatibility::COMPATIBLE)
            {
                return lux::cxx::unexpected(EFlowForgeCompileError::INCOMPATIBLE_BINDING);
            }
            result.binding_template.push_back({function.symbol_id, edge.target});
        }

        if (!lux::rdesc::validScriptDescription(result.description))
        {
            return lux::cxx::unexpected(EFlowForgeCompileError::INVALID_DESCRIPTION);
        }
        return result;
    }
}
