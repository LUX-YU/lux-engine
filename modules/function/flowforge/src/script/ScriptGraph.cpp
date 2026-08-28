#include <lux/engine/flowforge/script/ScriptGraph.hpp>

namespace lux::flowforge
{
    bool validFlowForgeExports(std::span<const ExportMethodNode> exports) noexcept
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
        return true;
    }
}
