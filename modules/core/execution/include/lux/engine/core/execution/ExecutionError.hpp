#pragma once

#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/core/execution/visibility.h>

namespace lux::execution
{
    enum class EExecutionError : std::uint8_t
    {
        INVALID_ARGUMENT,
        INVALID_STATE,
        CAPACITY_EXCEEDED,
        STOPPING,
        ALLOCATION_FAILURE,
        WORKER_CREATION_FAILURE,
        BACKEND_FAILURE,
        WRONG_THREAD,
        WORK_PENDING,
        ALREADY_JOINED,
        CAPABILITY_UNAVAILABLE,
    };


    /// Register the Process diagnostic catalog during host assembly.
    [[nodiscard]] LUX_CORE_EXECUTION_PUBLIC cxx::expected<void, error::Error> registerExecutionErrors() noexcept;

    /// Copy the stable diagnostic identity and code; no registration or allocation.
    [[nodiscard]] LUX_CORE_EXECUTION_PUBLIC error::Error toError(EExecutionError value) noexcept;
} // namespace lux::execution
