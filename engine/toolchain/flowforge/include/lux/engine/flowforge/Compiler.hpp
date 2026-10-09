#pragma once

#include <lux/engine/flowforge/FlowForgeFailure.hpp>
#include <lux/engine/flowforge/compiler/visibility.h>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/function/script/ScriptBindingHint.hpp>
#include <lux/engine/function/script/ScriptEvent.hpp>
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace lux::flowforge
{
    class FlowGraph;

    struct FlowForgeCompileOptions final
    {
        std::string module_name;
        std::filesystem::path linker;
        lux::rdesc::ScriptLifecycleRoles lifecycle;
        ScriptAbilityNodeCatalogView script_abilities;
        std::span<const lux::script::ScriptEventSourceDescription> script_events;
    };

    // CPU compilation output. No LLVM objects, source borrows, paths or process handles escape.
    struct FlowForgeObject final
    {
        std::vector<std::byte> object;
        lux::rdesc::Script description;
        std::string target_triple; // Fixed with the object bytes; retries cannot change their ABI.
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_COMPILER_PUBLIC FlowForgeResult<FlowForgeObject>
    compileFlowForgeObject(const FlowGraph&, const FlowForgeCompileOptions&) noexcept;

    // Blocking linker/file work. Failure retains the compiled object for a later retry.
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_COMPILER_PUBLIC FlowForgeResult<lux::script::ScriptArtifact> linkFlowForgeObject(
        const FlowForgeObject&,
        const std::filesystem::path& linker = {}
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_COMPILER_PUBLIC FlowForgeResult<lux::script::ScriptArtifact>
    compileFlowForgeScript(const FlowGraph& graph, FlowForgeCompileOptions options) noexcept;

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_COMPILER_PUBLIC FlowForgeResult<std::vector<lux::script::ScriptBindingHint>>
    describeFlowForgeBindingHints(const FlowGraph& graph) noexcept;
} // namespace lux::flowforge
