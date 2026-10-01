#pragma once
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <malloc.h>
#include <new>

// Test-only instrumentation: C++ new calls in this executable and its linked archives,
// on the measuring thread. It does not intercept another DLL's CRT, malloc or GPU allocations.
namespace allocation_probe
{
    inline thread_local bool enabled{};
    inline thread_local std::size_t calls{}, bytes{};
    inline void record(std::size_t size) noexcept
    {
        if (enabled) { ++calls; bytes += size; }
    }
    inline void begin() noexcept { calls = bytes = 0; enabled = true; }
    inline void end() noexcept
    {
        enabled = false;
        std::printf("1000 stable updates: owner-thread executable/static C++ new calls=%zu requested_bytes=%zu (not all DLL/CRT/GPU heap)\n", calls, bytes);
    }
}
void* operator new(std::size_t size)
{
    auto* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc{};
    allocation_probe::record(size);
    return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t size, std::align_val_t align)
{
    auto* p = _aligned_malloc(size ? size : 1, std::size_t(align));
    if (!p) throw std::bad_alloc{};
    allocation_probe::record(size);
    return p;
}
void* operator new[](std::size_t size, std::align_val_t align) { return ::operator new(size, align); }
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
