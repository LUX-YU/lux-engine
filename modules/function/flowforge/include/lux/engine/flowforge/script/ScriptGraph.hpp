#pragma once

#include <lux/engine/flowforge/visibility.h>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/function/script/ScriptSymbol.hpp>

#include <cstdint>

namespace lux::flowforge
{
    class FlowGraph;

    struct FlowForgeExportNodeId final
    {
        std::uint64_t value{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value != 0U;
        }

        friend bool operator==(FlowForgeExportNodeId, FlowForgeExportNodeId) noexcept = default;
    };

    struct ExportMethodNode final
    {
        FlowForgeExportNodeId id;
        lux::graph::NodeId entry_node_id{};
        lux::script::ScriptSymbolId symbol{};
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC
    bool validFlowForgeExports(const FlowGraph& graph) noexcept;
}
