#include "Allocation.hpp"
#include <cstdlib>
#include <new>
#include <malloc.h>

static void account() noexcept
{
    if (render_allocation::participate && render_allocation::window.load(std::memory_order_relaxed))
        render_allocation::calls.fetch_add(1, std::memory_order_relaxed);
}

void* operator new(std::size_t size)
{
    account();
    if (auto* result = std::malloc(size ? size : 1)) return result;
    std::abort();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

void* operator new(std::size_t size, std::align_val_t alignment)
{
    account();
    if (auto* result = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment))) return result;
    std::abort();
}

void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* value, std::align_val_t) noexcept { _aligned_free(value); }
void operator delete[](void* value, std::align_val_t) noexcept { _aligned_free(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { _aligned_free(value); }
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { _aligned_free(value); }
