#pragma once

#include <cstdint>

namespace lux::runtime::detail
{
    struct OwnerThreadCpuSample
    {
        std::uint64_t nanoseconds{0u};
        bool available{false};
    };

    [[nodiscard]] OwnerThreadCpuSample sampleOwnerThreadCpu() noexcept;

    [[nodiscard]] inline std::uint64_t elapsedOwnerThreadCpu(
        OwnerThreadCpuSample before,
        OwnerThreadCpuSample after,
        std::uint64_t wall_nanoseconds) noexcept
    {
        if (!before.available || !after.available ||
            after.nanoseconds < before.nanoseconds)
        {
            // Unsupported platforms remain fail-closed by using wall time.
            return wall_nanoseconds;
        }
        return after.nanoseconds - before.nanoseconds;
    }
} // namespace lux::runtime::detail
