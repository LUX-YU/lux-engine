#pragma once

#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/process/visibility.h>

namespace lux::process
{
    enum class EExecutionError : std::uint8_t;

    /// Register the Process diagnostic catalog during host assembly.
    [[nodiscard]] LUX_PROCESS_EXECUTION_PUBLIC cxx::expected<void, error::Error> registerExecutionErrors() noexcept;

    /// Copy the stable diagnostic identity and code; no registration or allocation.
    [[nodiscard]] LUX_PROCESS_EXECUTION_PUBLIC error::Error toError(EExecutionError value) noexcept;
} // namespace lux::process
