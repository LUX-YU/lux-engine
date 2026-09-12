#include "CostSample.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>

namespace er1_cost
{
    ProcessMemory processMemory()
    {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        assert(K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)));
        return {memory.PrivateUsage, memory.WorkingSetSize, memory.PeakWorkingSetSize};
    }
    std::uint64_t cycles()
    {
        ULONG64 result{};
        assert(QueryThreadCycleTime(GetCurrentThread(), &result));
        return result;
    }
    double cpu(bool process)
    {
        FILETIME created{}, ended{}, kernel{}, user{};
        const auto okay = process ? GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)
                                  : GetThreadTimes(GetCurrentThread(), &created, &ended, &kernel, &user);
        assert(okay);
        const auto ticks = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        return (ticks(kernel) + ticks(user)) * 1e-7;
    }
} // namespace er1_cost
