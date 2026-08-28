#pragma once

#include <lux/engine/flowforge/script/ScriptGraph.hpp>
#include <lux/engine/description/Script.hpp>
#include <lux/engine/flowforge/compiler/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <span>
#include <vector>

namespace lux::flowforge
{
    struct FlowForgeStateLayout final
    {
        std::uint64_t hash{};
        std::uint32_t size{};
        std::uint32_t align{1U};
        std::vector<std::byte> defaults;
    };

    struct FlowForgeAotAbiManifest final
    {
        std::uint32_t abi_version{};
        FlowForgeStateLayout state;
        std::vector<lux::script::ScriptSymbolId> symbols;
    };

    struct FlowForgeScriptArtifact final
    {
        lux::rdesc::Script description;
        FlowForgeAotAbiManifest abi;
    };

    enum class EFlowForgeCompileError : std::uint8_t
    {
        INVALID_MODULE_NAME,
        INVALID_STATE_LAYOUT,
        DUPLICATE_SYMBOL,
        INVALID_GRAPH,
        INVALID_DESCRIPTION,
        ALLOCATION_FAILURE,
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_SCRIPT_COMPILER_PUBLIC
    lux::cxx::expected<FlowForgeScriptArtifact, EFlowForgeCompileError>
    compileFlowForgeScript(
        std::string module_name,
        std::span<const ExportMethodNode> graph_exports,
        FlowForgeStateLayout state
    ) noexcept;
}
