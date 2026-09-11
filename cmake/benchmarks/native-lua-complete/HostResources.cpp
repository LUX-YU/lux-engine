#include "HostResources.hpp"
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#include <vector>
#include <cstdint>
#include <cstdio>
void reportNa1HostResources(const char *phase)
{
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    const bool process_read =
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&process),
                             sizeof(process)) != FALSE;
    const auto heap_count = GetProcessHeaps(0, nullptr);
    std::vector<HANDLE> heaps(heap_count);
    const auto heap_written = GetProcessHeaps(heap_count, heaps.data());
    std::uint64_t busy_bytes{}, busy_blocks{};
    bool heap_complete = heap_written == heap_count;
    for (const auto heap : heaps)
    {
        if (!heap_complete || !HeapLock(heap))
        {
            heap_complete = false;
            break;
        }
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heap, &entry))
        {
            if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
            {
                busy_bytes += entry.cbData;
                ++busy_blocks;
            }
        }
        heap_complete = GetLastError() == ERROR_NO_MORE_ITEMS;
        HeapUnlock(heap);
    }
    std::printf("PROCESS_MEMORY phase=%s observed=%d working_set=%zu private_commit=%zu peak_working_set=%zu "
                "heap_walk_complete=%d all_heaps_busy_bytes=%llu busy_blocks=%llu\n",
                phase, process_read, process.WorkingSetSize, process.PrivateUsage, process.PeakWorkingSetSize,
                heap_complete, busy_bytes, busy_blocks);
}
