#pragma once

// Test-only C++ allocation census; adapted from the accepted R3 benchmark.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace
{
    std::atomic<bool> measuring{};
    std::atomic<std::uint64_t> allocation_count{}, allocation_bytes{};

    void account(std::size_t bytes) noexcept
    {
        if (measuring.load(std::memory_order_relaxed))
        {
            allocation_count.fetch_add(1, std::memory_order_relaxed);
            allocation_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
} // namespace

void *operator new(std::size_t size)
{
    account(size);
    if (void *result = std::malloc(size ? size : 1))
        return result;
    std::abort();
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void *pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void *operator new(std::size_t size, std::align_val_t alignment)
{
    account(size);
#if defined(_WIN32)
    void *result = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment));
#else
    const auto align = static_cast<std::size_t>(alignment);
    void *result = std::aligned_alloc(align, (size + align - 1) / align * align);
#endif
    if (!result)
        std::abort();
    return result;
}

void *operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void operator delete(void *pointer, std::align_val_t) noexcept
{
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void operator delete[](void *pointer, std::align_val_t alignment) noexcept
{
    ::operator delete(pointer, alignment);
}

void operator delete(void *pointer, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(pointer, alignment);
}

void operator delete[](void *pointer, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(pointer, alignment);
}
