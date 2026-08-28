#include <lux/engine/flowforge/compiler/ScriptArtifactCompiler.hpp>

#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <algorithm>
#include <new>

namespace lux::flowforge
{
    lux::cxx::expected<FlowForgeScriptArtifact, EFlowForgeCompileError>
    compileFlowForgeScript(
        std::string module_name,
        std::span<const ExportMethodNode> graph_exports,
        FlowForgeStateLayout state
    ) noexcept
    {
        try
        {
            if (module_name.empty())
                return lux::cxx::unexpected(EFlowForgeCompileError::INVALID_MODULE_NAME);

            const bool is_invalid_state_size = state.defaults.size() > state.size;
            const bool is_invalid_state_alignment = state.align == 0U || (state.align & (state.align - 1U)) != 0U;
            if (is_invalid_state_size || is_invalid_state_alignment)
                return lux::cxx::unexpected(EFlowForgeCompileError::INVALID_STATE_LAYOUT);

            if (!validFlowForgeExports(graph_exports))
                return lux::cxx::unexpected(EFlowForgeCompileError::INVALID_GRAPH);

            FlowForgeScriptArtifact result;
            result.description.schema_version = lux::rdesc::Script::kSchemaVersion;
            result.description.module_name = std::move(module_name);
            result.description.body = lux::rdesc::NativeModuleScript{
                LUX_SCRIPT_ABI_VERSION,
                state.hash,
                state.size,
                state.align,
                state.defaults
            };
            result.abi.abi_version = LUX_SCRIPT_ABI_VERSION;
            result.abi.state = std::move(state);

            result.description.exports.reserve(graph_exports.size());
            result.abi.symbols.reserve(graph_exports.size());
            for (const auto& node : graph_exports)
            {
                const auto symbol = node.symbol;
                const bool is_duplicate_symbol = std::find(
                    result.abi.symbols.begin(),
                    result.abi.symbols.end(),
                    symbol
                ) != result.abi.symbols.end();
                if (symbol == lux::script::InvalidScriptSymbolId || is_duplicate_symbol)
                    return lux::cxx::unexpected(EFlowForgeCompileError::DUPLICATE_SYMBOL);

                lux::rdesc::ScriptFunction function;
                function.name = node.name;
                function.symbol_id = symbol;
                function.args = node.parameters;
                function.returns = node.returns;
                result.description.exports.push_back(std::move(function));
                result.abi.symbols.push_back(symbol);
            }

            if (!lux::rdesc::validScriptDescription(result.description))
                return lux::cxx::unexpected(EFlowForgeCompileError::INVALID_DESCRIPTION);

            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EFlowForgeCompileError::ALLOCATION_FAILURE);
        }
        catch (...)
        {
            return lux::cxx::unexpected(EFlowForgeCompileError::FOREIGN_EXCEPTION);
        }
    }
}
