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
        QUEUE_FULL,
        RECORDING_BUSY,
        RESOURCE_PREPARATION_PENDING
    };

    // Owner-thread snapshot for one actual execution context. Replaced contexts start at zero.
    // Native failures remain errors; these counters describe only successful decisions.
    struct ExecutionProgress final
    {
        std::array<std::uint64_t, 5> counts{};
        std::uint32_t last_submitted_slot = UINT32_MAX; // Submission observation, not proof of GPU completion.

        [[nodiscard]] std::uint64_t count(EExecutionOutcome outcome) const noexcept
        {
            return counts[static_cast<std::size_t>(outcome)];
        }
    };
} // namespace lux::render
