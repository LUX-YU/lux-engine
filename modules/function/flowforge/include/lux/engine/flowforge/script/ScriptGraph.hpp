#pragma once

#include <lux/engine/flowforge/visibility.h>
#include <lux/engine/description/Script.hpp>

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

        friend bool operator==(FlowForgeExportNodeId, FlowForgeExportNodeId) noexcept = default;
    };

    struct ExportMethodNode final
    {
        FlowForgeExportNodeId id;
        lux::script::ScriptSymbolId symbol{};
        std::string name;
        std::vector<lux::rdesc::ScriptValueType> parameters;
        std::vector<lux::rdesc::ScriptValueType> returns;
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC
    bool validFlowForgeExports(std::span<const ExportMethodNode> exports) noexcept;
}
