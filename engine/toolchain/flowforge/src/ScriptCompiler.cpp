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
        std::string module_name,
        lux::rdesc::EScriptModel model,
        std::span<const TypedEntryNode> graph_exports,
        FlowForgeStateLayout state
    )
    {
        if (module_name.empty())
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_MODULE_NAME
            );
        }
        if (state.defaults.size() > state.size || state.align == 0U ||
            (state.align & (state.align - 1U)) != 0U)
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_STATE_LAYOUT
            );
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
            const auto kind_name = [&]() noexcept -> std::string_view
            {
                switch (node.kind)
                {
                case ETypedEntryKind::HOOK: return "hook";
                case ETypedEntryKind::EVENT: return "event";
                case ETypedEntryKind::LIFECYCLE: return "lifecycle";
                }
                return "unknown";
            }();
            std::vector<lux::script::ScriptSemanticType> parameters =
                node.parameters;
            std::vector<lux::script::ScriptSemanticType> returns =
                node.returns;
            const auto symbol = lux::script::scriptSymbolId(
                result.description.module_name,
                node.member,
                {parameters, returns}
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
            function.name = node.system_instance + "." +
                std::string{kind_name} + "." + node.member;
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
        if (!lux::rdesc::validScriptDescription(result.description))
        {
            return lux::cxx::unexpected(
                EFlowForgeCompileError::INVALID_DESCRIPTION
            );
        }
        return result;
    }
}
