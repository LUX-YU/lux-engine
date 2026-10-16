#include "TypedFixture.hpp"
#include <memory>

namespace ipc_typed_test
{
    namespace
    {
        struct AllocationHeader final
        {
            void* base;
            std::uint64_t origin;
        };

        std::uint64_t module_origin = 0;
        AllocationOriginCounts module_counts;
    }

    void setAllocationOrigin(std::uint64_t origin) noexcept { module_origin = origin; }

    void clearAllocationOriginCounts() noexcept { module_counts = {}; }

    AllocationOriginCounts allocationOriginCounts() noexcept { return module_counts; }

    void* allocateOriginObject(std::size_t size, std::size_t alignment) noexcept
    {
        auto* base = std::malloc(size + sizeof(AllocationHeader) + alignment - 1);
        if (!base) std::abort();
        const auto start = reinterpret_cast<std::uintptr_t>(base) + sizeof(AllocationHeader);
        const auto address = (start + alignment - 1) & ~(alignment - 1);
        auto* header = reinterpret_cast<AllocationHeader*>(address - sizeof(AllocationHeader));
        std::construct_at(header, AllocationHeader{base, module_origin});
        ++module_counts.allocations;
        return reinterpret_cast<void*>(address);
    }

    void releaseOriginObject(void* value) noexcept
    {
        if (!value) return;
        const auto address = reinterpret_cast<std::uintptr_t>(value) - sizeof(AllocationHeader);
        auto* header = reinterpret_cast<AllocationHeader*>(address);
        ++module_counts.deletions;
        if (header->origin != module_origin) ++module_counts.wrong_origin;
        // Record mismatches without an invalid free: this fixture's original malloc base is always released.
        auto* base = header->base;
        std::destroy_at(header);
        std::free(base);
    }
}
