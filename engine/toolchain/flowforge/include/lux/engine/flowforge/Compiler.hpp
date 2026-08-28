#pragma once

#include <lux/engine/flowforge/graph/StateLayout.hpp>
#include <lux/engine/flowforge/script/ScriptGraph.hpp>
#include <lux/engine/flowforge/compiler/visibility.h>
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <span>
#include <string>

namespace lux::flowforge
{
    enum class EFlowForgeCompileError : std::uint8_t
    {
        INVALID_MODULE_NAME,
        INVALID_STATE_LAYOUT,
        DUPLICATE_SYMBOL,
        INVALID_GRAPH,
        INVALID_DESCRIPTION,
        ALLOCATION_FAILURE,
        FOREIGN_EXCEPTION,
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_COMPILER_PUBLIC
    lux::cxx::expected<lux::script::ScriptArtifact, EFlowForgeCompileError>
    compileFlowForgeScript(
        std::string module_name,
        std::span<const ExportMethodNode> graph_exports,
        const StateLayout& state
    ) noexcept;
}
