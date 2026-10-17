#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lux::render
{
    enum class EExecutionOutcome : std::uint8_t
    {
        EXECUTED,
        PREVIOUS_SUBMISSION_PENDING,
        READBACK_HELD,
        QUEUE_FULL,
        RECORDING_BUSY
    };

    // Owner-thread snapshot for one actual execution context. Replaced contexts start at zero.
    // Native failures remain errors; these counters describe only successful decisions.
    struct ExecutionProgress final
    {
        std::array<std::uint64_t, 5> counts{};

        [[nodiscard]] std::uint64_t count(EExecutionOutcome outcome) const noexcept
        {
            return counts[static_cast<std::size_t>(outcome)];
        }
    };
}
