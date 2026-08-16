#pragma once
/**
 * @file AsyncStatistics.hpp
 * @brief Allocation-free latency histogram vocabulary for AsyncRuntime.
 */

#include <array>
#include <cstddef>
#include <cstdint>

namespace lux::exec
{
    inline constexpr std::size_t kAsyncLatencyBucketCount = 12u;
    using AsyncLatencyHistogram =
        std::array<std::uint64_t, kAsyncLatencyBucketCount>;

    namespace detail
    {
        /// Upper bounds in nanoseconds. The final bucket is overflow (>1 s).
        inline constexpr std::array<
            std::uint64_t,
            kAsyncLatencyBucketCount - 1u> kAsyncLatencyUpperBoundsNs{
                1'000u,
                4'000u,
                16'000u,
                64'000u,
                256'000u,
                1'000'000u,
                4'000'000u,
                16'000'000u,
                64'000'000u,
                256'000'000u,
                1'000'000'000u};

        [[nodiscard]] inline std::size_t asyncLatencyBucket(
            std::uint64_t nanoseconds) noexcept
        {
            for (std::size_t index = 0u;
                 index < kAsyncLatencyUpperBoundsNs.size();
                 ++index)
            {
                if (nanoseconds <= kAsyncLatencyUpperBoundsNs[index])
                    return index;
            }
            return kAsyncLatencyBucketCount - 1u;
        }
    }
}
