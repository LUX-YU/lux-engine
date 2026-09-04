#include <lux/engine/simulation/scripting/detail/BoundedFrameStorage.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace
{
    std::atomic_size_t allocations{};
    std::atomic_bool count_allocations{};
}

int main()
{
    using lux::simulation::script::detail::BoundedFrameStorage;
    auto created = BoundedFrameStorage::create(256U, 4U, 64U);
    assert(created);
    auto storage = std::move(*created);

    count_allocations.store(true, std::memory_order_release);
    auto first = storage.acquire(24U, 8U);
    auto second = storage.acquire(48U, 16U);
    auto third = storage.acquire(64U, 32U);
    count_allocations.store(false, std::memory_order_release);
    assert(first && second && third);
    assert(reinterpret_cast<std::uintptr_t>(first->data) % 8U == 0U);
    assert(reinterpret_cast<std::uintptr_t>(second->data) % 16U == 0U);
    assert(reinterpret_cast<std::uintptr_t>(third->data) % 32U == 0U);
    assert(allocations.load(std::memory_order_acquire) == 0U);
    assert(storage.stats().active_frames == 3U);
    assert(storage.release(*second));
    auto replacement = storage.acquire(32U, 16U);
    assert(replacement);
    assert(storage.release(*replacement));
    assert(storage.release(*first));
    assert(storage.release(*third));
    assert(!storage.release(*third));
    assert(storage.stats().active_frames == 0U);
    assert(storage.stats().frame_high_water == 3U);

    const auto oversized = storage.acquire(257U, 8U);
    assert(!oversized);
    assert(storage.stats().capacity_failures == 1U);
    return 0;
}

void* operator new(std::size_t size)
{
    if (count_allocations.load(std::memory_order_relaxed))
        allocations.fetch_add(1U, std::memory_order_relaxed);
    if (void* result = std::malloc(size == 0U ? 1U : size))
        return result;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* value) noexcept
{
    std::free(value);
}

void operator delete[](void* value) noexcept
{
    ::operator delete(value);
}

void operator delete(void* value, std::size_t) noexcept
{
    ::operator delete(value);
}

void operator delete[](void* value, std::size_t) noexcept
{
    ::operator delete(value);
}
