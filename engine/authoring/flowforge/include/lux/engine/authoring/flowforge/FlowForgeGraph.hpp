#pragma once

#include <lux/engine/authoring/flowforge/visibility.h>
#include <lux/engine/description/Script.hpp>
#include <lux/engine/simulation/ScriptMountDescription.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lux::flowforge
{
    struct FlowForgeExportNodeId final
    {
        std::uint64_t value{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value != 0U;
        }

        friend bool operator==(
            FlowForgeExportNodeId,
            FlowForgeExportNodeId
        ) noexcept = default;
    };

    struct ExportMethodNode final
    {
        FlowForgeExportNodeId id;
        lux::script::ScriptSymbolId symbol{};
        std::string name;
        std::vector<lux::rdesc::ScriptValueType> parameters;
        std::vector<lux::rdesc::ScriptValueType> returns;
    };

    struct BindingEdge final
    {
        FlowForgeExportNodeId export_node;
        lux::simulation::ScriptBindingTarget target;
    };

    [[nodiscard]] LUX_ENGINE_AUTHORING_FLOWFORGE_PUBLIC
    bool validFlowForgeGraph(
        std::span<const ExportMethodNode> exports,
        std::span<const BindingEdge> bindings
    ) noexcept;
}
