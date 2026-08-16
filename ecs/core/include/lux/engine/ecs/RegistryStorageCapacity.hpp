#pragma once
/**
 * @file RegistryStorageCapacity.hpp
 * @brief Checked EnTT storage-capacity reservations.
 *
 * EnTT storage::reserve() reserves its packed entity array and component
 * payload capacity. It does not promise to allocate every sparse entity-index
 * page which a later emplace may touch. Consequently this helper is an
 * overflow-safe packed-capacity admission primitive.  The sparse-write budget
 * helpers below complement it: when their result is armed through the
 * registry-owned allocator, arbitrary/recycled entity indices are covered by
 * a no-grow publication transaction as well.
 */

#include <lux/cxx/compile_time/expected.hpp>

#include <entt/entt.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lux::ecs
{
    enum class EStorageCapacityReservationError : std::uint8_t
    {
        SIZE_OVERFLOW,
        CAPACITY_UNSATISFIED
    };

    enum class ERegistryPublicationBudgetError : std::uint8_t
    {
        SIZE_OVERFLOW
    };

    using RegistryPublicationBudgetResult = lux::cxx::expected<
        std::size_t,
        ERegistryPublicationBudgetError>;

    /// One component insertion can allocate one entity sparse page and can
    /// grow the vector which owns sparse-page pointers.  Entity indices are
    /// bounded by entt_traits::entity_mask, so both allocations have a finite
    /// upper bound.  The factor of two covers the largest vector growth policy
    /// used by the supported MSVC and libc++ toolchains; the strict allocator
    /// test is the executable tripwire if that implementation contract moves.
    inline constexpr std::size_t kSparsePointerGrowthFactor = 2u;

    [[nodiscard]] constexpr std::size_t registryMaximumSparsePages() noexcept
    {
        constexpr auto entity_count =
            static_cast<std::size_t>(
                entt::entt_traits<entt::entity>::entity_mask) + 1u;
        constexpr auto page_size =
            entt::entt_traits<entt::entity>::page_size;
        return (entity_count + page_size - 1u) / page_size;
    }

    /// Conservative registry-allocator bytes for `writes` insertions into one
    /// already-created and packed/payload-reserved EnTT storage.  At most one
    /// sparse page can be created by each insertion and no storage can own
    /// more than registryMaximumSparsePages() pages.
    [[nodiscard]] inline RegistryPublicationBudgetResult
    registrySparsePublicationBytes(std::size_t writes) noexcept
    {
        constexpr auto maximum_pages = registryMaximumSparsePages();
        constexpr auto sparse_page_bytes =
            entt::entt_traits<entt::entity>::page_size *
            sizeof(entt::entity);
        constexpr auto pointer_table_bytes =
            maximum_pages * sizeof(entt::entity*) *
            kSparsePointerGrowthFactor;
        constexpr auto alignment_slack =
            (alignof(entt::entity) - 1u) +
            (alignof(entt::entity*) - 1u);
        constexpr auto bytes_per_sparse_event =
            sparse_page_bytes + pointer_table_bytes + alignment_slack;
        const auto events = std::min(writes, maximum_pages);
        if (events > std::numeric_limits<std::size_t>::max() /
                bytes_per_sparse_event)
        {
            return lux::cxx::unexpected(
                ERegistryPublicationBudgetError::SIZE_OVERFLOW);
        }
        return events * bytes_per_sparse_event;
    }

    struct StorageCapacityReservation final
    {
        std::size_t size_before{0u};
        std::size_t capacity_before{0u};
        std::size_t requested_capacity{0u};
        std::size_t capacity_after{0u};

        [[nodiscard]] bool preventsPackedGrowth(
            std::size_t additional) const noexcept
        {
            return additional <=
                    std::numeric_limits<std::size_t>::max() - size_before &&
                capacity_after >= size_before + additional;
        }
    };

    using StorageCapacityReservationResult = lux::cxx::expected<
        StorageCapacityReservation,
        EStorageCapacityReservationError>;

    [[nodiscard]] inline bool checkedAdditionalCapacity(
        std::size_t current,
        std::size_t additional,
        std::size_t& result) noexcept
    {
        if (additional >
            std::numeric_limits<std::size_t>::max() - current)
        {
            return false;
        }
        result = current + additional;
        return true;
    }

    [[nodiscard]] inline bool addRegistryPublicationBytes(
        std::size_t current,
        std::size_t additional,
        std::size_t& result) noexcept
    {
        return checkedAdditionalCapacity(current, additional, result);
    }

    template <class Storage>
    [[nodiscard]] StorageCapacityReservationResult reserveStorageCapacity(
        Storage& storage,
        std::size_t requested_capacity)
    {
        StorageCapacityReservation result{
            storage.size(),
            storage.capacity(),
            requested_capacity,
            storage.capacity()};
        storage.reserve(requested_capacity);
        result.capacity_after = storage.capacity();
        if (result.capacity_after < requested_capacity)
        {
            return lux::cxx::unexpected(
                EStorageCapacityReservationError::CAPACITY_UNSATISFIED);
        }
        return result;
    }

    template <class Storage>
    [[nodiscard]] StorageCapacityReservationResult
    reserveAdditionalStorageCapacity(
        Storage& storage,
        std::size_t additional)
    {
        std::size_t requested = 0u;
        if (!checkedAdditionalCapacity(
                storage.size(), additional, requested))
        {
            return lux::cxx::unexpected(
                EStorageCapacityReservationError::SIZE_OVERFLOW);
        }
        return reserveStorageCapacity(storage, requested);
    }
} // namespace lux::ecs
