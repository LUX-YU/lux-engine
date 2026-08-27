#include <lux/engine/flowforge/script/ScriptGraph.hpp>

#include <algorithm>

namespace lux::flowforge
{
    bool validFlowForgeGraph(std::span<const ExportMethodNode> exports, std::span<const BindingEdge> bindings) noexcept
    {
        for (std::size_t index{}; index < exports.size(); ++index)
        {
            if (!exports[index].id ||
                exports[index].symbol == lux::script::InvalidScriptSymbolId ||
                exports[index].name.empty())
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (exports[previous].id == exports[index].id)
                    return false;
                if (exports[previous].symbol == exports[index].symbol)
                    return false;
            }
        }
        for (std::size_t index{}; index < bindings.size(); ++index)
        {
            if (!bindings[index].export_node ||
                std::none_of(exports.begin(), exports.end(), [&](const ExportMethodNode& value) noexcept {
                    return value.id == bindings[index].export_node;
                }))
            {
                return false;
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (bindings[previous].export_node == bindings[index].export_node &&
                    bindings[previous].target == bindings[index].target)
                {
                    return false;
                }
            }
        }
        return true;
    }
}
