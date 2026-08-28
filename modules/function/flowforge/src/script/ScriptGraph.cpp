#include <lux/engine/flowforge/script/ScriptGraph.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>

#include <new>
#include <unordered_set>

namespace lux::flowforge
{
    bool validFlowForgeExports(const FlowGraph& graph) noexcept
    {
        try
        {
            std::unordered_set<std::uint64_t> export_ids;
            std::unordered_set<std::uint64_t> entry_node_ids;
            std::unordered_set<lux::script::ScriptSymbolId> symbols;
            export_ids.reserve(graph.exports().size());
            entry_node_ids.reserve(graph.exports().size());
            symbols.reserve(graph.exports().size());

            for (const auto& exported : graph.exports())
            {
                const Node* entry = graph.findNodeById(exported.entry_node_id);
                const bool is_invalid_identity = !exported.id || exported.entry_node_id == 0U ||
                    exported.symbol == lux::script::InvalidScriptSymbolId;
                const bool is_duplicate_identity = !export_ids.insert(exported.id.value).second ||
                    !entry_node_ids.insert(exported.entry_node_id).second || !symbols.insert(exported.symbol).second;
                const bool is_invalid_entry = entry == nullptr || entry->operation() != ENodeOperation::ON_EVENT;
                if (is_invalid_identity || is_duplicate_identity || is_invalid_entry)
                {
                    return false;
                }
            }
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }
}
