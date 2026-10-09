#pragma once

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <string>

namespace lux::flowforge
{
    enum class EFlowForgeError : std::uint8_t
    {
        GRAPH_INVALID,
        INVALID_MODULE_NAME,
        INVALID_DESCRIPTION,
        FOREIGN_EXCEPTION = 4, // Preserve existing error values; former recoverable OOM code is retired.
        CONTEXT_CREATION_FAILED,
        IR_VERIFICATION_FAILED,
        LOWERING_FAILED,
        JIT_ENGINE_CREATION_FAILED,
        JIT_SYMBOL_LOOKUP_FAILED,
        JIT_INVOCATION_FAILED,
        AOT_CODEGEN_FAILED,
        LINK_FAILED,
        IO_FAILED,
        UNKNOWN_SCRIPT_ABILITY_CONTRACT,
        UNKNOWN_SCRIPT_ABILITY_METHOD,
        SCRIPT_ABILITY_SCHEMA_MISMATCH,
        SCRIPT_ABILITY_REQUIREMENT_CONFLICT,
        UNKNOWN_SCRIPT_EVENT_SOURCE,
        SCRIPT_EVENT_SCHEMA_MISMATCH,
        UNSUPPORTED_SCRIPT_ABILITY_TYPE,
        BORROWED_VALUE_CROSSES_SUSPENSION,
        ASYNC_LIFECYCLE_NOT_SUPPORTED,
        INVALID_CONTINUATION_FRAME_LAYOUT,
        UNSUPPORTED_COROUTINE_CONTROL_FLOW,
    };

    struct FlowForgeFailure final
    {
        EFlowForgeError code{EFlowForgeError::GRAPH_INVALID};
        std::string message;
        std::uint64_t node_id{};
        std::uint64_t pin_id{};
    };

    template <class Value> using FlowForgeResult = lux::cxx::expected<Value, FlowForgeFailure>;
} // namespace lux::flowforge
