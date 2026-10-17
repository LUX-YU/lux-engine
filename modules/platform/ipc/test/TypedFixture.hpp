#pragma once

#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <array>
#include <cstdlib>
#include <vector>

namespace ipc_typed_test
{
    struct Batch final
    {
        std::uint64_t scene = 0;
        std::vector<std::uint64_t> values;
    };

    struct AllocationOriginCounts final
    {
        std::uint64_t allocations = 0;
        std::uint64_t deletions = 0;
        std::uint64_t wrong_origin = 0;
    };

    // Compiled independently in EXE and DLL. Class-specific allocators expose the allocation origin
    // even when both modules' normal test instrumentation happens to use compatible malloc/free.
    void setAllocationOrigin(std::uint64_t origin) noexcept;
    void clearAllocationOriginCounts() noexcept;
    AllocationOriginCounts allocationOriginCounts() noexcept;
    void* allocateOriginObject(std::size_t size, std::size_t alignment) noexcept;
    void releaseOriginObject(void* value) noexcept;

    template <std::size_t Size, std::size_t Alignment>
    struct alignas(Alignment) AllocationOriginProbe final
    {
        std::array<std::uint64_t, Size / sizeof(std::uint64_t)> fields{};

        explicit AllocationOriginProbe(std::uint64_t value) noexcept { fields.fill(value); }
        AllocationOriginProbe(AllocationOriginProbe&&) noexcept = default;
        AllocationOriginProbe& operator=(AllocationOriginProbe&&) noexcept = default;

        static void* operator new(std::size_t size) { return allocateOriginObject(size, Alignment); }
        static void* operator new(std::size_t size, std::align_val_t) { return allocateOriginObject(size, Alignment); }
        static void operator delete(void* value) noexcept { releaseOriginObject(value); }
        static void operator delete(void* value, std::align_val_t) noexcept { releaseOriginObject(value); }
    };

    using LargeOriginProbe = AllocationOriginProbe<128, 16>;
    using AlignedOriginProbe = AllocationOriginProbe<64, 64>;
}
