#include "Allocation.hpp"
#include <algorithm>
#include <cstdlib>
#include <new>
#include <malloc.h>

namespace ipc_allocation
{
    thread_local bool watching = false;
    thread_local std::size_t calls = 0;
    void start() noexcept { calls = 0; watching = true; }
    std::size_t stop() noexcept { watching = false; return calls; }
    void record() noexcept { if (watching) ++calls; }
}

void* operator new(std::size_t size)
{
    ipc_allocation::record();
    auto* result = std::malloc((std::max)(size, std::size_t{1}));
    if (!result) std::abort();
    return result;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* value) noexcept { std::free(value); }

void operator delete[](void* value) noexcept { std::free(value); }

void operator delete(void* value, std::size_t) noexcept { std::free(value); }

void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

void* operator new(std::size_t size, std::align_val_t alignment)
{
    ipc_allocation::record();
    auto* result = _aligned_malloc((std::max)(size, std::size_t{1}), static_cast<std::size_t>(alignment));
    if (!result) std::abort();
    return result;
}

void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }

void operator delete(void* value, std::align_val_t) noexcept { _aligned_free(value); }

void operator delete[](void* value, std::align_val_t) noexcept { _aligned_free(value); }

void operator delete(void* value, std::size_t, std::align_val_t) noexcept { _aligned_free(value); }

void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { _aligned_free(value); }
