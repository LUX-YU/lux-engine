#include <lux/engine/flowforge/Compiler.hpp>

#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <new>
#include <unordered_set>

namespace lux::flowforge
{
    lux::cxx::expected<lux::script::ScriptArtifact, EFlowForgeCompileError>
    compileFlowForgeScript(
        std::string module_name,
        std::span<const ExportMethodNode> graph_exports,
        const StateLayout& state
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

            lux::rdesc::Script description;
            description.schema_version = lux::rdesc::Script::kSchemaVersion;
            description.module_name = std::move(module_name);
            description.body = lux::rdesc::NativeModuleScript{
                LUX_SCRIPT_ABI_VERSION,
                state.hash,
                state.size,
                state.align,
                state.defaults
            };

            description.exports.reserve(graph_exports.size());
            std::unordered_set<lux::script::ScriptSymbolId> symbols;
            symbols.reserve(graph_exports.size());
            for (const auto& node : graph_exports)
            {
                const auto symbol = node.symbol;
                const bool is_duplicate_symbol = !symbols.insert(symbol).second;
                if (symbol == lux::script::InvalidScriptSymbolId || is_duplicate_symbol)
                    return lux::cxx::unexpected(EFlowForgeCompileError::DUPLICATE_SYMBOL);

                lux::rdesc::ScriptFunction function;
                function.name = node.name;
                function.symbol_id = symbol;
                function.args = node.parameters;
                function.returns = node.returns;
                description.exports.push_back(std::move(function));
            }

            if (!lux::rdesc::validScriptDescription(description))
                return lux::cxx::unexpected(EFlowForgeCompileError::INVALID_DESCRIPTION);

            auto artifact = lux::script::ScriptArtifact::create(std::move(description), {});
            if (!artifact)
            {
                const auto error = artifact.error() == lux::script::EScriptArtifactError::ALLOCATION_FAILURE
                    ? EFlowForgeCompileError::ALLOCATION_FAILURE
                    : EFlowForgeCompileError::INVALID_DESCRIPTION;
                return lux::cxx::unexpected(error);
            }
            return std::move(*artifact);
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
