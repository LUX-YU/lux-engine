#include "ClientAllocation.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace client_allocation
{
    namespace
    {
        std::atomic<bool> watching{false};
        std::atomic<std::size_t> calls{0};
        std::atomic<std::size_t> bytes{0};
    }

    void start() noexcept
    {
        calls.store(0, std::memory_order_relaxed);
        bytes.store(0, std::memory_order_relaxed);
        watching.store(true, std::memory_order_release);
    }

    Counts stop() noexcept
    {
        watching.store(false, std::memory_order_release);
        return {calls.load(std::memory_order_relaxed), bytes.load(std::memory_order_relaxed)};
    }

    void record(std::size_t size) noexcept
    {
        if (!watching.load(std::memory_order_acquire)) return;
        calls.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(size, std::memory_order_relaxed);
    }
}

void* operator new(std::size_t size)
{
    client_allocation::record(size);
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
    client_allocation::record(size);
    auto* result = _aligned_malloc((std::max)(size, std::size_t{1}), static_cast<std::size_t>(alignment));
    if (!result) std::abort();
    return result;
}

void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }

void operator delete(void* value, std::align_val_t) noexcept { _aligned_free(value); }

void operator delete[](void* value, std::align_val_t) noexcept { _aligned_free(value); }

void operator delete(void* value, std::size_t, std::align_val_t) noexcept { _aligned_free(value); }

void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { _aligned_free(value); }
