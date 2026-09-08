#pragma once

#include <cstddef>
#include <cstdlib>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <memory>
#include <string>

namespace lux::editor::editing::detail
{
    enum class EEditAllocationSite : std::uint8_t
    {
        FACTORY,
        ENTRIES,
        RECLAIM,
        LABEL
    };
    enum class EEditContract : std::uint8_t
    {
        HISTORY_LIFETIME,
        ROUTE_LIFETIME,
        REGISTRATION_LIFETIME
    };

    // Test-only callbacks execute at allocation boundaries, before the real allocation.
    // A diagnostic build may inject bad_alloc here; no hook changes the public object layout.
    struct EditDiagnostics final
    {
        void (*allocation)(EEditAllocationSite, std::size_t){};
        void (*contract)(EEditContract) noexcept {};
    };
    struct EditHistoryTestAccess;

#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
    struct EditAllocationStatistics final
    {
        std::size_t live_bytes{}, live_allocations{}, live_objects{};
        bool operator==(const EditAllocationStatistics&) const noexcept = default;
    };
    LUX_EDITOR_EDITING_PUBLIC EditAllocationStatistics& allocationStatistics() noexcept;
    LUX_EDITOR_EDITING_PUBLIC EditDiagnostics& editDiagnostics() noexcept;
    struct LUX_EDITOR_EDITING_PUBLIC EditHistoryTestAccess final
    {
        static void counters(
            EditHistory&, std::uint64_t serial, std::uint64_t revision, std::uint64_t event, std::uint64_t request
        ) noexcept;
        static void identityCounter(std::uint64_t value) noexcept;
    };
#endif

    inline void allocationCheckpoint(EEditAllocationSite site, std::size_t bytes)
    {
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
        if (const auto callback = editDiagnostics().allocation)
        {
            callback(site, bytes);
        }
#else
        static_cast<void>(site);
        static_cast<void>(bytes);
#endif
    }

    [[noreturn]] inline void failEditContract(EEditContract reason) noexcept
    {
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
        if (const auto callback = editDiagnostics().contract)
        {
            callback(reason);
        }
#else
        static_cast<void>(reason);
#endif
        std::abort();
    }

    template <class T, EEditAllocationSite Site> struct EditAllocator
    {
        using value_type = T;
        using is_always_equal = std::true_type;
        template <class U> struct rebind
        {
            using other = EditAllocator<U, Site>;
        };
        EditAllocator() noexcept = default;
        template <class U> EditAllocator(const EditAllocator<U, Site>&) noexcept
        {
        }
        [[nodiscard]] T* allocate(std::size_t count)
        {
            allocationCheckpoint(Site, count * sizeof(T));
            auto* pointer = std::allocator<T>{}.allocate(count);
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
            allocationStatistics().live_bytes += count * sizeof(T);
            ++allocationStatistics().live_allocations;
#endif
            return pointer;
        }
        void deallocate(T* pointer, std::size_t count) noexcept
        {
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
            allocationStatistics().live_bytes -= count * sizeof(T);
            --allocationStatistics().live_allocations;
#endif
            std::allocator<T>{}.deallocate(pointer, count);
        }
        template <class U> bool operator==(const EditAllocator<U, Site>&) const noexcept
        {
            return true;
        }
    };
    using EditLabel = std::basic_string<char, std::char_traits<char>, EditAllocator<char, EEditAllocationSite::LABEL>>;
} // namespace lux::editor::editing::detail
