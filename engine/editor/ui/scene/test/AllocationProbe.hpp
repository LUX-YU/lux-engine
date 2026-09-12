#pragma once
#include <memory>
namespace inspector_fixture
{
    inline bool reject_allocation{};
    inline unsigned rejected_allocations{};
    template<class T> struct AllocationProbe
    {
        using value_type = T;
        AllocationProbe() = default;
        template<class U> AllocationProbe(const AllocationProbe<U> &) noexcept {}
        T *allocate(std::size_t count)
        {
            if (reject_allocation)
            {
                ++rejected_allocations;
                throw std::bad_alloc{};
            }
            return std::allocator<T>{}.allocate(count);
        }
        void deallocate(T *value, std::size_t count) noexcept { std::allocator<T>{}.deallocate(value, count); }
        friend bool operator==(const AllocationProbe &, const AllocationProbe &) = default;
    };
}
