#pragma once

#include <lux/engine/flowforge/compiler/visibility.h>
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace lux::flowforge
{
    class FlowGraph;

    enum class EFlowForgeError : std::uint8_t
    {
        GRAPH_INVALID,
        INVALID_MODULE_NAME,
        INVALID_DESCRIPTION,
        ALLOCATION_FAILURE,
        FOREIGN_EXCEPTION,
        CONTEXT_CREATION_FAILED,
        IR_VERIFICATION_FAILED,
        LOWERING_FAILED,
        JIT_ENGINE_CREATION_FAILED,
        JIT_SYMBOL_LOOKUP_FAILED,
        JIT_INVOCATION_FAILED,
        AOT_CODEGEN_FAILED,
        LINK_FAILED,
        IO_FAILED,
    };

    struct FlowForgeFailure final
    {
        EFlowForgeError code{EFlowForgeError::GRAPH_INVALID};
        std::string message;
        std::uint64_t node_id{};
        std::uint64_t pin_id{};
    };

    template <class Value>
    using FlowForgeResult = lux::cxx::expected<Value, FlowForgeFailure>;

    struct FlowForgeCompileOptions final
    {
        std::string module_name;
        std::filesystem::path linker;
        lux::rdesc::ScriptLifecycleRoles lifecycle;
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_COMPILER_PUBLIC
    FlowForgeResult<lux::script::ScriptArtifact>
    compileFlowForgeScript(
        const FlowGraph& graph,
        FlowForgeCompileOptions options
    ) noexcept;
}
