#include "OwnerThreadCpuClock.hpp"

#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <time.h>
#endif

namespace lux::runtime::detail
{
#if defined(_WIN32)
    namespace
    {
        [[nodiscard]] double threadCyclesPerNanosecond() noexcept
        {
            static const double value = []() noexcept
            {
                LARGE_INTEGER frequency{};
                if (!QueryPerformanceFrequency(&frequency) ||
                    frequency.QuadPart <= 0)
                {
                    return 0.0;
                }

                double best = 0.0;
                const auto target_ticks = std::max<LONGLONG>(
                    frequency.QuadPart / 1000,
                    1);
                for (std::uint32_t sample = 0u; sample < 4u; ++sample)
                {
                    ULONG64 before_cycles = 0u;
                    ULONG64 after_cycles = 0u;
                    LARGE_INTEGER before_counter{};
                    LARGE_INTEGER after_counter{};
                    QueryPerformanceCounter(&before_counter);
                    if (!QueryThreadCycleTime(
                            GetCurrentThread(),
                            &before_cycles))
                    {
                        return 0.0;
                    }
                    do
                    {
                        QueryPerformanceCounter(&after_counter);
                    } while (after_counter.QuadPart -
                            before_counter.QuadPart < target_ticks);
                    if (!QueryThreadCycleTime(
                            GetCurrentThread(),
                            &after_cycles) ||
                        after_cycles <= before_cycles)
                    {
                        return 0.0;
                    }
                    QueryPerformanceCounter(&after_counter);
                    const auto elapsed_counter =
                        after_counter.QuadPart - before_counter.QuadPart;
                    if (elapsed_counter <= 0)
                        continue;
                    const auto elapsed_nanoseconds =
                        static_cast<double>(elapsed_counter) *
                        1'000'000'000.0 /
                        static_cast<double>(frequency.QuadPart);
                    best = std::max(
                        best,
                        static_cast<double>(
                            after_cycles - before_cycles) /
                            elapsed_nanoseconds);
                }
                return best;
            }();
            return value;
        }
    } // namespace
#endif

    OwnerThreadCpuSample sampleOwnerThreadCpu() noexcept
    {
#if defined(_WIN32)
        const auto cycles_per_nanosecond = threadCyclesPerNanosecond();
        ULONG64 cycles = 0u;
        if (cycles_per_nanosecond <= 0.0 ||
            !QueryThreadCycleTime(GetCurrentThread(), &cycles))
            return {};
        return OwnerThreadCpuSample{
            static_cast<std::uint64_t>(
                static_cast<double>(cycles) /
                cycles_per_nanosecond),
            true};
#elif defined(CLOCK_THREAD_CPUTIME_ID)
        timespec value{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
            return {};
        return OwnerThreadCpuSample{
            static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000u +
                static_cast<std::uint64_t>(value.tv_nsec),
            true};
#else
        return {};
#endif
    }
} // namespace lux::runtime::detail
