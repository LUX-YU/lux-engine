#pragma once

#include <lux/engine/process/visibility.h>

#include <cstdint>

namespace lux::process::detail::testing
{
    enum class ETimerCreateFailure : std::uint8_t
    {
        NONE,
        ALLOCATION,
        WORKER,
        BACKEND
    };

    LUX_PROCESS_EXECUTION_PUBLIC void failNextTimerCreate(ETimerCreateFailure failure) noexcept;
    [[nodiscard]] LUX_PROCESS_EXECUTION_PUBLIC ETimerCreateFailure consumeTimerCreateFailure() noexcept;
}
