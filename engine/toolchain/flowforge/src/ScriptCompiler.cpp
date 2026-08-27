#include <lux/engine/toolchain/flowforge/ScriptCompiler.hpp>

#include <lux/engine/function/script/abi/lux_script_abi.h>

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
        const lux::simulation::SimulationDescription& simulation,
        std::string module_name,
        FlowForgeStateLayout state
    )
    {
        if (module_name.empty())
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_MODULE_NAME
            );
        }
        if (state.defaults.size() > state.size)
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_STATE_LAYOUT
            );
        }

        FlowForgeScriptArtifact result;
        result.description.schema_version = lux::rdesc::Script::kSchemaVersion;
        result.description.module_name = std::move(module_name);
        result.description.body = lux::rdesc::NativeModuleScript{
            LUX_SCRIPT_ABI_VERSION,
            state.hash,
            state.size,
            state.defaults};
        result.abi.abi_version = LUX_SCRIPT_ABI_VERSION;
        result.abi.state = std::move(state);

        for (const auto& node : makeTypedEntryCatalog(simulation))
        {
            const auto kind_name = node.kind == ETypedEntryKind::HOOK
                ? "hook"
                : "event";
            const auto canonical_symbol =
                result.description.module_name + ":" + node.system_instance +
                ":" + kind_name + ":" + node.member;
            const auto symbol = lux::script::scriptSemanticTypeId(
                canonical_symbol
            );
            if (symbol == lux::script::InvalidScriptSymbolId ||
                std::find(
                    result.abi.symbols.begin(),
                    result.abi.symbols.end(),
                    symbol
                ) != result.abi.symbols.end())
            {
                return lux::cxx::unexpected(
                    EFlowForgeCompileError::DUPLICATE_SYMBOL
                );
            }
            lux::rdesc::ScriptFunction function;
            function.name = node.system_instance + "." + kind_name + "." +
                node.member;
            function.symbol_id = symbol;
            function.args.reserve(node.parameters.size());
            function.returns.reserve(node.returns.size());
            for (const auto& parameter : node.parameters)
                function.args.push_back(copyType(parameter));
            for (const auto& return_type : node.returns)
                function.returns.push_back(copyType(return_type));
            result.description.exports.push_back(std::move(function));
            result.description.default_bindings.push_back(
                lux::rdesc::ScriptBindingDescription{
                    symbol,
                    node.kind == ETypedEntryKind::HOOK
                        ? lux::rdesc::EScriptBindingKind::HOOK
                        : lux::rdesc::EScriptBindingKind::EVENT,
                    node.system_type,
                    node.system_instance,
                    node.member}
            );
            result.abi.symbols.push_back(symbol);
        }
        if (!lux::rdesc::validScriptDescription(result.description))
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_DESCRIPTION
            );
        }
        return result;
    }
}
