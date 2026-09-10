#include <vk_mem_alloc.h>
#include <cstdint>

// Runs inside the DLL that owns VMA; neither a second allocator nor a public SDK entry point.
extern "C" __declspec(dllexport) void
lux_er1_render_memory_statistics(VmaAllocator allocator, std::uint64_t *bytes, std::uint64_t *allocations) noexcept
{
    VmaTotalStatistics measured{};
    vmaCalculateStatistics(allocator, &measured);
    *bytes = measured.total.statistics.allocationBytes;
    *allocations = measured.total.statistics.allocationCount;
}
