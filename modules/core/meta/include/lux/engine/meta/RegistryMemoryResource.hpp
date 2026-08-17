#pragma once
/**
 * @file RegistryMemoryResource.hpp
 * @brief Registry-owned allocation and no-grow publication transactions.
 *
 * EnTT propagates the allocator supplied to basic_registry into entity and
 * component sparse sets, pool/group maps, signals and registry context.  This
 * resource is the engine allocator endpoint for that graph.
 *
 * A publication reservation owns a private upstream block.  Entering it makes
 * that block the only allocation source until the returned scope is destroyed.
 * Exhausting the block is an always-on engine invariant: the resource records
 * the violation and terminates instead of falling back to its upstream.
 * Separate reservations therefore remain isolated when several batches are
 * armed for the same command barrier.
 *
 * The resource is owner-thread only.  Blocks carry allocation/live-byte
 * counters in their own header, so a normal or committed block is reclaimed
 * as soon as its last EnTT allocation is released without publication-time
 * bookkeeping. Unused committed tails are reusable by later normal growth;
 * repeated publication therefore stays bounded by live/high-water storage,
 * not by the number of historical batches.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/visibility.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::meta
{
    class RegistryMemoryResource;
    class RegistryPublicationReservation;

    /// Closes reservation admission for one complete publication barrier.
    /// Pre-armed reservations may still be entered while this scope is live,
    /// but a re-entrant reservePublication() fails closed without consulting
    /// the upstream allocator. Owner-thread only, like the resource itself.
    class LUX_CORE_PUBLIC RegistryPublicationAdmissionScope final
    {
    public:
        RegistryPublicationAdmissionScope() noexcept = default;
        ~RegistryPublicationAdmissionScope() noexcept;

        RegistryPublicationAdmissionScope(
            RegistryPublicationAdmissionScope&& other) noexcept;
        RegistryPublicationAdmissionScope& operator=(
            RegistryPublicationAdmissionScope&& other) noexcept;

        RegistryPublicationAdmissionScope(
            const RegistryPublicationAdmissionScope&) = delete;
        RegistryPublicationAdmissionScope& operator=(
            const RegistryPublicationAdmissionScope&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return resource_ != nullptr;
        }

    private:
        friend class RegistryMemoryResource;

        explicit RegistryPublicationAdmissionScope(
            std::shared_ptr<RegistryMemoryResource> resource) noexcept;

        void reset() noexcept;

        std::shared_ptr<RegistryMemoryResource> resource_;
    };

    /// Non-throwing upstream used only while arming/growing registry-owned
    /// blocks.  Publication scopes never call it.
    class LUX_CORE_PUBLIC IRegistryMemoryUpstream
    {
    public:
        virtual ~IRegistryMemoryUpstream() = default;

        [[nodiscard]] virtual void* tryAllocate(
            std::size_t bytes,
            std::size_t alignment) noexcept = 0;
        virtual void deallocate(
            void* pointer,
            std::size_t bytes,
            std::size_t alignment) noexcept = 0;

    protected:
        IRegistryMemoryUpstream() = default;
    };

    [[nodiscard]] LUX_CORE_PUBLIC IRegistryMemoryUpstream&
    defaultRegistryMemoryUpstream() noexcept;

    enum class ERegistryPublicationReservationError : std::uint8_t
    {
        SIZE_OVERFLOW,
        UPSTREAM_EXHAUSTED,
        PUBLICATION_ACTIVE
    };

    struct RegistryMemorySnapshot final
    {
        std::uint64_t upstream_allocation_calls{0u};
        std::uint64_t upstream_allocation_bytes{0u};
        std::uint64_t upstream_deallocation_calls{0u};
        std::uint64_t upstream_deallocation_bytes{0u};
        std::uint64_t live_upstream_blocks{0u};
        std::uint64_t live_upstream_bytes{0u};
        std::uint64_t peak_upstream_blocks{0u};
        std::uint64_t peak_upstream_bytes{0u};
        std::uint64_t allocator_requests{0u};
        std::uint64_t allocator_bytes{0u};
        std::uint64_t publication_allocator_requests{0u};
        std::uint64_t publication_allocator_bytes{0u};
        std::uint64_t publication_invariant_failures{0u};
        std::uint64_t armed_reservations{0u};
        std::uint64_t active_scopes{0u};
        std::uint64_t committed_reservations{0u};
        std::uint64_t cancelled_reservations{0u};
    };

    class LUX_CORE_PUBLIC RegistryPublicationScope final
    {
    public:
        RegistryPublicationScope() noexcept = default;
        ~RegistryPublicationScope() noexcept;

        RegistryPublicationScope(RegistryPublicationScope&& other) noexcept;
        RegistryPublicationScope& operator=(
            RegistryPublicationScope&& other) noexcept;

        RegistryPublicationScope(const RegistryPublicationScope&) = delete;
        RegistryPublicationScope& operator=(
            const RegistryPublicationScope&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return token_ != nullptr;
        }

    private:
        friend class RegistryMemoryResource;
        friend class RegistryPublicationReservation;

        RegistryPublicationScope(
            std::shared_ptr<RegistryMemoryResource> resource,
            void* token) noexcept;

        void reset() noexcept;

        std::shared_ptr<RegistryMemoryResource> resource_;
        void* token_{nullptr};
    };

    class LUX_CORE_PUBLIC RegistryPublicationReservation final
    {
    public:
        RegistryPublicationReservation() noexcept = default;
        ~RegistryPublicationReservation() noexcept;

        RegistryPublicationReservation(
            RegistryPublicationReservation&& other) noexcept;
        RegistryPublicationReservation& operator=(
            RegistryPublicationReservation&& other) noexcept;

        RegistryPublicationReservation(
            const RegistryPublicationReservation&) = delete;
        RegistryPublicationReservation& operator=(
            const RegistryPublicationReservation&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return token_ != nullptr;
        }

        [[nodiscard]] std::size_t capacityBytes() const noexcept;

        /// Enters the no-grow publication scope.  A reservation can be
        /// entered once.  Destroying the returned scope commits its block to
        /// registry ownership; destroying an unentered reservation cancels it.
        [[nodiscard]] RegistryPublicationScope enter() noexcept;

    private:
        friend class RegistryMemoryResource;

        RegistryPublicationReservation(
            std::shared_ptr<RegistryMemoryResource> resource,
            void* token) noexcept;

        void reset() noexcept;

        std::shared_ptr<RegistryMemoryResource> resource_;
        void* token_{nullptr};
    };

    template <typename T>
    using RegistryPublicationExp = lux::cxx::expected<T, ERegistryPublicationReservationError>;

    using RegistryPublicationReservationResult = RegistryPublicationExp<RegistryPublicationReservation>;

    class LUX_CORE_PUBLIC RegistryMemoryResource final
        : public std::enable_shared_from_this<RegistryMemoryResource>
    {
    public:
        /// The caller-owned upstream must outlive the returned resource.
        [[nodiscard]] static std::shared_ptr<RegistryMemoryResource> create(IRegistryMemoryUpstream* upstream = nullptr);

        ~RegistryMemoryResource() noexcept;
        RegistryMemoryResource(const RegistryMemoryResource&) = delete;
        RegistryMemoryResource& operator=(
            const RegistryMemoryResource&) = delete;

        [[nodiscard]] RegistryPublicationReservationResult
        reservePublication(std::size_t bytes) noexcept;

        [[nodiscard]] RegistryPublicationAdmissionScope
        closePublicationAdmission() noexcept;

        [[nodiscard]] RegistryMemorySnapshot snapshot() const noexcept;

        [[nodiscard]] void* allocate(
            std::size_t bytes,
            std::size_t alignment);

        void deallocate(
            void* pointer,
            std::size_t bytes,
            std::size_t alignment) noexcept;

    private:
        friend class RegistryPublicationReservation;
        friend class RegistryPublicationScope;
        friend class RegistryPublicationAdmissionScope;

        struct Impl;

        explicit RegistryMemoryResource(IRegistryMemoryUpstream& upstream);

        [[nodiscard]] std::size_t reservationCapacity(
            const void* token) const noexcept;
        [[nodiscard]] void* enterReservation(void* token) noexcept;
        void cancelReservation(void* token) noexcept;
        void leaveReservation(void* token) noexcept;
        void reopenPublicationAdmission() noexcept;

        std::unique_ptr<Impl> impl_;
    };

    template <class Type>
    class RegistryAllocator final
    {
    public:
        using value_type = Type;
        using propagate_on_container_move_assignment = std::true_type;
        using is_always_equal = std::false_type;

        RegistryAllocator()
            : resource_(RegistryMemoryResource::create())
        {}

        explicit RegistryAllocator(
            std::shared_ptr<RegistryMemoryResource> resource) noexcept
            : resource_(std::move(resource))
        {
            if (!resource_)
                std::abort();
        }

        template <class Other>
        RegistryAllocator(const RegistryAllocator<Other>& other) noexcept
            : resource_(other.resource())
        {}

        [[nodiscard]] Type* allocate(std::size_t count)
        {
            if (count > static_cast<std::size_t>(-1) / sizeof(Type))
                std::abort();
            return static_cast<Type*>(resource_->allocate(
                count * sizeof(Type), alignof(Type)));
        }

        void deallocate(Type* pointer, std::size_t count) noexcept
        {
            if (count > static_cast<std::size_t>(-1) / sizeof(Type))
                std::abort();
            resource_->deallocate(
                pointer, count * sizeof(Type), alignof(Type));
        }

        [[nodiscard]] const std::shared_ptr<RegistryMemoryResource>&
        resource() const noexcept
        {
            return resource_;
        }

        template <class Other>
        [[nodiscard]] bool operator==(
            const RegistryAllocator<Other>& other) const noexcept
        {
            return resource_ == other.resource();
        }

    private:
        template <class>
        friend class RegistryAllocator;

        std::shared_ptr<RegistryMemoryResource> resource_;
    };
} // namespace lux::meta
