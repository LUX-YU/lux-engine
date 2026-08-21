#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::script
{
    enum class EScriptError : std::uint8_t
    {
        INVALID_ARGUMENT,
        DUPLICATE_BACKEND,
        DUPLICATE_EXTENSION,
        BACKEND_NOT_FOUND,
        UNSUPPORTED_EXTENSION,
        IO_ERROR,
        LOAD_FAILED,
        MEMORY_LOAD_UNSUPPORTED,
        INVALID_ENTRY_POINT,
        INVALID_MODULE,
        ABI_MISMATCH,
        HOST_BIND_FAILED,
        DUPLICATE_MODULE_NAME,
        MODULE_NOT_FOUND,
        FUNCTION_NOT_FOUND,
        STALE_HANDLE,
        INVOKE_FAILED,
        HANDLE_EXHAUSTED
    };

    struct LUX_FUNCTION_PUBLIC ScriptFailure
    {
        EScriptError code = EScriptError::INVALID_ARGUMENT;
        std::string  detail;

        friend bool operator==(const ScriptFailure&, const ScriptFailure&) = default;
    };

    template<class T>
    using ScriptResult = lux::cxx::expected<T, ScriptFailure>;

    [[nodiscard]] inline ScriptFailure scriptFailure(
        EScriptError code,
        std::string detail
    )
    {
        return ScriptFailure{code, std::move(detail)};
    }
}
