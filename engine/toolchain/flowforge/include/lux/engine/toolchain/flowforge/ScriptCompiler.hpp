#pragma once

#include <lux/engine/authoring/flowforge/FlowForgeGraph.hpp>
#include <lux/engine/description/Script.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/toolchain/flowforge/visibility.h>

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
        std::vector<lux::simulation::ScriptBindingDescription>
            binding_template;
    };

    enum class EFlowForgeCompileError : std::uint8_t
    {
        INVALID_MODULE_NAME,
        INVALID_STATE_LAYOUT,
        DUPLICATE_SYMBOL,
        INVALID_GRAPH,
        INCOMPATIBLE_BINDING,
        INVALID_DESCRIPTION,
    };

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_FLOWFORGE_PUBLIC
    lux::cxx::expected<FlowForgeScriptArtifact, EFlowForgeCompileError>
    compileFlowForgeScript(
        std::string module_name,
        lux::rdesc::EScriptModel model,
        std::span<const ExportMethodNode> graph_exports,
        std::span<const BindingEdge> graph_bindings,
        const lux::simulation::SimulationDescription& simulation,
        FlowForgeStateLayout state
    );
}
