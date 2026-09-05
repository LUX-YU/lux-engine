#include <lux/engine/simulation/scripting/detail/BoundedFrameStorage.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace
{
    std::size_t fail_at{};
    std::size_t requests{};
    std::size_t arena_deletes{};
    void* arena{};
    bool arena_freed{};

    bool rejectAllocation() noexcept
    {
        return fail_at != 0U && ++requests == fail_at;
    }

    bool check(bool condition, const char* message) noexcept
    {
        if (!condition)
            std::fprintf(stderr, "%s\n", message);
        return condition;
    }
}

void* operator new(std::size_t size)
{
    if (rejectAllocation())
        throw std::bad_alloc{};
    if (auto* result = std::malloc(size == 0U ? 1U : size))
        return result;
    throw std::bad_alloc{};
}

void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    if (rejectAllocation())
        return nullptr;
#ifdef _MSC_VER
    arena = _aligned_malloc(size, static_cast<std::size_t>(alignment));
#else
    const auto align = static_cast<std::size_t>(alignment);
    arena = std::aligned_alloc(align, (size + align - 1U) / align * align);
#endif
    arena_freed = false;
    return arena;
}

void operator delete(void* value, std::align_val_t) noexcept
{
    if (value == nullptr)
        return;
    if (value == arena)
    {
        ++arena_deletes;
        // Detect the old double-owner bug without invoking undefined behavior in the test allocator.
        if (arena_freed)
            return;
        arena_freed = true;
    }
#ifdef _MSC_VER
    _aligned_free(value);
#else
    std::free(value);
#endif
}

int main(int argc, char** argv)
{
    using lux::simulation::script::detail::BoundedFrameStorage;
    using lux::simulation::script::detail::EBoundedFrameStorageError;
    fail_at = argc == 2 ? static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10)) : 0U;
    bool valid = true;
    {
        auto created = BoundedFrameStorage::create(4096U, 8U, 64U);
        fail_at = 0U;
        if (argc == 2)
        {
            valid &= check(!created, "injected factory allocation did not fail");
            if (!created)
                valid &= check(created.error() == EBoundedFrameStorageError::ALLOCATION_FAILURE,
                    "wrong injected failure classification");
        }
        else
        {
            valid &= check(static_cast<bool>(created), "factory failed");
            if (created)
            {
                auto moved = std::move(*created);
                BoundedFrameStorage assigned;
                assigned = std::move(moved);
                const auto allocation = assigned.acquire(48U, 32U);
                valid &= check(static_cast<bool>(allocation), "moved storage cannot allocate");
                if (allocation)
                    valid &= check(assigned.release(*allocation), "moved storage cannot release");
            }
        }
    }
    const std::size_t expected_deletes = arena != nullptr ? 1U : 0U;
    valid &= check(arena_deletes == expected_deletes, "arena ownership released other than exactly once");
    std::printf("requests=%zu arena_deletes=%zu expected=%zu\n", requests, arena_deletes, expected_deletes);
    return valid ? 0 : 1;
}
