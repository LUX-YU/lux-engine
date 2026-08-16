#include <lux/engine/meta/RegistryMemoryResource.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <utility>

namespace lux::meta
{
    namespace
    {
        constexpr std::size_t kBlockStorageAlignment = 4096u;

        [[nodiscard]] bool checkedAdd(
            std::size_t left,
            std::size_t right,
            std::size_t& result) noexcept
        {
            if (right > std::numeric_limits<std::size_t>::max() - left)
                return false;
            result = left + right;
            return true;
        }

        [[nodiscard]] std::byte* alignForward(
            std::byte* pointer,
            std::size_t alignment) noexcept
        {
            const auto address = reinterpret_cast<std::uintptr_t>(pointer);
            const auto remainder = address % alignment;
            if (remainder == 0u)
                return pointer;
            return pointer + (alignment - remainder);
        }

        [[noreturn]] void registryMemoryInvariantFailed() noexcept
        {
            std::abort();
        }

        class NewDeleteRegistryMemoryUpstream final
            : public IRegistryMemoryUpstream
        {
        public:
            [[nodiscard]] void* tryAllocate(
                std::size_t bytes,
                std::size_t alignment) noexcept override
            {
                return ::operator new(
                    bytes,
                    std::align_val_t{alignment},
                    std::nothrow);
            }

            void deallocate(
                void* pointer,
                std::size_t,
                std::size_t alignment) noexcept override
            {
                ::operator delete(
                    pointer, std::align_val_t{alignment});
            }
        };
    } // namespace

    struct RegistryMemoryResource::Impl final
    {
        enum class EBlockState : std::uint8_t
        {
            NORMAL,
            ARMED,
            ACTIVE,
            COMMITTED
        };

        struct Block final
        {
            Block* previous{nullptr};
            Block* next{nullptr};
            std::size_t upstream_bytes{0u};
            std::size_t capacity{0u};
            std::size_t used{0u};
            std::size_t live_allocations{0u};
            std::size_t live_bytes{0u};
            EBlockState state{EBlockState::NORMAL};
        };

        explicit Impl(IRegistryMemoryUpstream& upstream_value) noexcept
            : upstream(&upstream_value), owner(std::this_thread::get_id())
        {}

        void requireOwner() const noexcept
        {
            if (owner != std::this_thread::get_id())
                registryMemoryInvariantFailed();
        }

        [[nodiscard]] Block* tryAllocateBlock(
            std::size_t capacity,
            EBlockState state,
            ERegistryPublicationReservationError& error) noexcept
        {
            std::size_t total = 0u;
            if (!checkedAdd(sizeof(Block), capacity, total) ||
                !checkedAdd(
                    total, kBlockStorageAlignment - 1u, total))
            {
                error = ERegistryPublicationReservationError::SIZE_OVERFLOW;
                return nullptr;
            }

            void* raw = upstream->tryAllocate(
                total, alignof(std::max_align_t));
            if (!raw)
            {
                error = ERegistryPublicationReservationError::
                    UPSTREAM_EXHAUSTED;
                return nullptr;
            }
            ++snapshot.upstream_allocation_calls;
            snapshot.upstream_allocation_bytes += total;
            if (snapshot.live_upstream_blocks ==
                    std::numeric_limits<std::uint64_t>::max() ||
                total > std::numeric_limits<std::uint64_t>::max() -
                    snapshot.live_upstream_bytes)
            {
                upstream->deallocate(
                    raw, total, alignof(std::max_align_t));
                registryMemoryInvariantFailed();
            }
            ++snapshot.live_upstream_blocks;
            snapshot.live_upstream_bytes += total;
            snapshot.peak_upstream_blocks = std::max(
                snapshot.peak_upstream_blocks,
                snapshot.live_upstream_blocks);
            snapshot.peak_upstream_bytes = std::max(
                snapshot.peak_upstream_bytes,
                snapshot.live_upstream_bytes);
            auto* block = ::new (raw) Block{};
            block->upstream_bytes = total;
            block->capacity = capacity;
            block->state = state;
            link(block);
            return block;
        }

        void releaseBlock(Block* block) noexcept
        {
            if (block->live_allocations != 0u ||
                block->live_bytes != 0u ||
                snapshot.live_upstream_blocks == 0u ||
                snapshot.live_upstream_bytes < block->upstream_bytes)
            {
                registryMemoryInvariantFailed();
            }
            unlink(block);
            const auto bytes = block->upstream_bytes;
            block->~Block();
            upstream->deallocate(
                block, bytes, alignof(std::max_align_t));
            ++snapshot.upstream_deallocation_calls;
            snapshot.upstream_deallocation_bytes += bytes;
            --snapshot.live_upstream_blocks;
            snapshot.live_upstream_bytes -= bytes;
        }

        [[nodiscard]] std::byte* storageBegin(Block& block) const noexcept
        {
            auto* first = reinterpret_cast<std::byte*>(&block + 1u);
            return alignForward(first, kBlockStorageAlignment);
        }

        [[nodiscard]] void* allocateFrom(
            Block& block,
            std::size_t bytes,
            std::size_t alignment) noexcept
        {
            auto* begin = storageBegin(block);
            auto* current = begin + block.used;
            auto* aligned = alignForward(current, alignment);
            const auto padding = static_cast<std::size_t>(aligned - current);
            if (padding > block.capacity - block.used ||
                bytes > block.capacity - block.used - padding)
            {
                return nullptr;
            }
            block.used += padding + bytes;
            if (block.live_allocations ==
                    std::numeric_limits<std::size_t>::max() ||
                bytes > std::numeric_limits<std::size_t>::max() -
                    block.live_bytes)
            {
                registryMemoryInvariantFailed();
            }
            ++block.live_allocations;
            block.live_bytes += bytes;
            return aligned;
        }

        [[nodiscard]] Block* containingBlock(const void* pointer) const
            noexcept
        {
            const auto address = reinterpret_cast<std::uintptr_t>(pointer);
            for (auto* block = head; block; block = block->next)
            {
                const auto begin = reinterpret_cast<std::uintptr_t>(
                    storageBegin(*block));
                if (address >= begin && address - begin < block->used)
                    return block;
            }
            return nullptr;
        }

        [[nodiscard]] void* tryAllocateReusable(
            std::size_t bytes,
            std::size_t alignment) noexcept
        {
            for (auto* block = tail; block; block = block->previous)
            {
                if (block->state == EBlockState::NORMAL ||
                    block->state == EBlockState::COMMITTED)
                {
                    if (auto* result = allocateFrom(
                            *block, bytes, alignment))
                    {
                        return result;
                    }
                }
            }
            return nullptr;
        }

        void link(Block* block) noexcept
        {
            block->previous = tail;
            if (tail)
                tail->next = block;
            else
                head = block;
            tail = block;
        }

        void unlink(Block* block) noexcept
        {
            if (block->previous)
                block->previous->next = block->next;
            else
                head = block->next;
            if (block->next)
                block->next->previous = block->previous;
            else
                tail = block->previous;
            block->previous = nullptr;
            block->next = nullptr;
        }

        IRegistryMemoryUpstream* upstream{nullptr};
        std::thread::id owner;
        Block* head{nullptr};
        Block* tail{nullptr};
        Block* active{nullptr};
        bool publication_admission_closed{false};
        RegistryMemorySnapshot snapshot{};
    };

    RegistryPublicationAdmissionScope::
        RegistryPublicationAdmissionScope(
        std::shared_ptr<RegistryMemoryResource> resource) noexcept
        : resource_(std::move(resource))
    {}

    RegistryPublicationAdmissionScope::
        ~RegistryPublicationAdmissionScope() noexcept
    {
        reset();
    }

    RegistryPublicationAdmissionScope::
        RegistryPublicationAdmissionScope(
        RegistryPublicationAdmissionScope&& other) noexcept
        : resource_(std::move(other.resource_))
    {}

    RegistryPublicationAdmissionScope&
    RegistryPublicationAdmissionScope::operator=(
        RegistryPublicationAdmissionScope&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            resource_ = std::move(other.resource_);
        }
        return *this;
    }

    void RegistryPublicationAdmissionScope::reset() noexcept
    {
        if (resource_)
            resource_->reopenPublicationAdmission();
        resource_.reset();
    }

    RegistryPublicationScope::RegistryPublicationScope(
        std::shared_ptr<RegistryMemoryResource> resource,
        void* token) noexcept
        : resource_(std::move(resource)), token_(token)
    {}

    RegistryPublicationScope::~RegistryPublicationScope() noexcept
    {
        reset();
    }

    RegistryPublicationScope::RegistryPublicationScope(
        RegistryPublicationScope&& other) noexcept
        : resource_(std::move(other.resource_)),
          token_(std::exchange(other.token_, nullptr))
    {}

    RegistryPublicationScope& RegistryPublicationScope::operator=(
        RegistryPublicationScope&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            resource_ = std::move(other.resource_);
            token_ = std::exchange(other.token_, nullptr);
        }
        return *this;
    }

    void RegistryPublicationScope::reset() noexcept
    {
        if (token_)
            resource_->leaveReservation(token_);
        token_ = nullptr;
        resource_.reset();
    }

    RegistryPublicationReservation::RegistryPublicationReservation(
        std::shared_ptr<RegistryMemoryResource> resource,
        void* token) noexcept
        : resource_(std::move(resource)), token_(token)
    {}

    RegistryPublicationReservation::~RegistryPublicationReservation() noexcept
    {
        reset();
    }

    RegistryPublicationReservation::RegistryPublicationReservation(
        RegistryPublicationReservation&& other) noexcept
        : resource_(std::move(other.resource_)),
          token_(std::exchange(other.token_, nullptr))
    {}

    RegistryPublicationReservation&
    RegistryPublicationReservation::operator=(
        RegistryPublicationReservation&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            resource_ = std::move(other.resource_);
            token_ = std::exchange(other.token_, nullptr);
        }
        return *this;
    }

    std::size_t RegistryPublicationReservation::capacityBytes() const noexcept
    {
        return token_ ? resource_->reservationCapacity(token_) : 0u;
    }

    RegistryPublicationScope RegistryPublicationReservation::enter() noexcept
    {
        if (!token_)
            registryMemoryInvariantFailed();
        auto* token = resource_->enterReservation(token_);
        token_ = nullptr;
        auto resource = std::move(resource_);
        return RegistryPublicationScope{std::move(resource), token};
    }

    void RegistryPublicationReservation::reset() noexcept
    {
        if (token_)
            resource_->cancelReservation(token_);
        token_ = nullptr;
        resource_.reset();
    }

    IRegistryMemoryUpstream& defaultRegistryMemoryUpstream() noexcept
    {
        static NewDeleteRegistryMemoryUpstream upstream;
        return upstream;
    }

    std::shared_ptr<RegistryMemoryResource> RegistryMemoryResource::create(
        IRegistryMemoryUpstream* upstream)
    {
        if (!upstream)
            upstream = &defaultRegistryMemoryUpstream();
        return std::shared_ptr<RegistryMemoryResource>{
            new RegistryMemoryResource{*upstream}};
    }

    RegistryMemoryResource::RegistryMemoryResource(
        IRegistryMemoryUpstream& upstream)
        : impl_(std::make_unique<Impl>(upstream))
    {}

    RegistryMemoryResource::~RegistryMemoryResource() noexcept
    {
        impl_->requireOwner();
        if (impl_->active || impl_->snapshot.active_scopes != 0u ||
            impl_->publication_admission_closed ||
            impl_->snapshot.armed_reservations != 0u)
        {
            registryMemoryInvariantFailed();
        }
        while (impl_->tail)
            impl_->releaseBlock(impl_->tail);
    }

    RegistryPublicationReservationResult
    RegistryMemoryResource::reservePublication(std::size_t bytes) noexcept
    {
        impl_->requireOwner();
        if (impl_->active || impl_->snapshot.active_scopes != 0u ||
            impl_->publication_admission_closed)
        {
            return lux::cxx::unexpected(
                ERegistryPublicationReservationError::PUBLICATION_ACTIVE);
        }
        auto error = ERegistryPublicationReservationError::SIZE_OVERFLOW;
        auto* block = impl_->tryAllocateBlock(
            bytes, Impl::EBlockState::ARMED, error);
        if (!block)
            return lux::cxx::unexpected(error);
        ++impl_->snapshot.armed_reservations;
        return RegistryPublicationReservation{
            shared_from_this(), block};
    }

    RegistryPublicationAdmissionScope
    RegistryMemoryResource::closePublicationAdmission() noexcept
    {
        impl_->requireOwner();
        if (impl_->publication_admission_closed)
            registryMemoryInvariantFailed();
        impl_->publication_admission_closed = true;
        return RegistryPublicationAdmissionScope{shared_from_this()};
    }

    RegistryMemorySnapshot RegistryMemoryResource::snapshot() const noexcept
    {
        impl_->requireOwner();
        return impl_->snapshot;
    }

    void* RegistryMemoryResource::allocate(
        std::size_t bytes,
        std::size_t alignment)
    {
        impl_->requireOwner();
        if (bytes == 0u)
            bytes = 1u;
        if (alignment == 0u ||
            (alignment & (alignment - 1u)) != 0u)
        {
            registryMemoryInvariantFailed();
        }

        ++impl_->snapshot.allocator_requests;
        impl_->snapshot.allocator_bytes += bytes;
        if (impl_->active)
        {
            ++impl_->snapshot.publication_allocator_requests;
            impl_->snapshot.publication_allocator_bytes += bytes;
            if (auto* result = impl_->allocateFrom(
                    *impl_->active, bytes, alignment))
            {
                return result;
            }
            ++impl_->snapshot.publication_invariant_failures;
            registryMemoryInvariantFailed();
        }

        // A barrier-wide gate permits only allocations backed by the
        // reservation currently entered above. A zero-byte command which
        // performs an allocating registry mutation is a false budget claim,
        // never permission to fall through to reusable or upstream storage.
        if (impl_->publication_admission_closed)
        {
            ++impl_->snapshot.publication_invariant_failures;
            registryMemoryInvariantFailed();
        }

        if (auto* result = impl_->tryAllocateReusable(bytes, alignment))
            return result;

        std::size_t capacity = 0u;
        if (!checkedAdd(bytes, alignment - 1u, capacity))
            registryMemoryInvariantFailed();
        auto error = ERegistryPublicationReservationError::SIZE_OVERFLOW;
        auto* block = impl_->tryAllocateBlock(
            capacity, Impl::EBlockState::NORMAL, error);
        if (!block)
            registryMemoryInvariantFailed();
        auto* result = impl_->allocateFrom(*block, bytes, alignment);
        if (!result)
            registryMemoryInvariantFailed();
        return result;
    }

    void RegistryMemoryResource::deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t) noexcept
    {
        impl_->requireOwner();
        if (!pointer)
            return;
        if (bytes == 0u)
            bytes = 1u;
        auto* block = impl_->containingBlock(pointer);
        if (!block || block->live_allocations == 0u ||
            block->live_bytes < bytes ||
            block->state == Impl::EBlockState::ARMED)
        {
            registryMemoryInvariantFailed();
        }
        --block->live_allocations;
        block->live_bytes -= bytes;
        if (block->live_allocations == 0u)
        {
            if (block->live_bytes != 0u)
                registryMemoryInvariantFailed();
            if (block->state == Impl::EBlockState::NORMAL ||
                block->state == Impl::EBlockState::COMMITTED)
            {
                impl_->releaseBlock(block);
            }
        }
    }

    std::size_t RegistryMemoryResource::reservationCapacity(
        const void* token) const noexcept
    {
        impl_->requireOwner();
        const auto* block = static_cast<const Impl::Block*>(token);
        if (!block || block->state != Impl::EBlockState::ARMED)
            registryMemoryInvariantFailed();
        return block->capacity;
    }

    void* RegistryMemoryResource::enterReservation(void* token) noexcept
    {
        impl_->requireOwner();
        auto* block = static_cast<Impl::Block*>(token);
        if (!block || block->state != Impl::EBlockState::ARMED ||
            impl_->active || impl_->snapshot.active_scopes != 0u ||
            impl_->snapshot.armed_reservations == 0u)
        {
            registryMemoryInvariantFailed();
        }
        block->state = Impl::EBlockState::ACTIVE;
        impl_->active = block;
        --impl_->snapshot.armed_reservations;
        ++impl_->snapshot.active_scopes;
        return block;
    }

    void RegistryMemoryResource::cancelReservation(void* token) noexcept
    {
        impl_->requireOwner();
        auto* block = static_cast<Impl::Block*>(token);
        if (!block || block->state != Impl::EBlockState::ARMED ||
            impl_->snapshot.armed_reservations == 0u ||
            block->live_allocations != 0u)
        {
            registryMemoryInvariantFailed();
        }
        --impl_->snapshot.armed_reservations;
        ++impl_->snapshot.cancelled_reservations;
        impl_->releaseBlock(block);
    }

    void RegistryMemoryResource::leaveReservation(void* token) noexcept
    {
        impl_->requireOwner();
        auto* block = static_cast<Impl::Block*>(token);
        if (!block || block != impl_->active ||
            block->state != Impl::EBlockState::ACTIVE ||
            impl_->snapshot.active_scopes != 1u)
        {
            registryMemoryInvariantFailed();
        }
        impl_->active = nullptr;
        --impl_->snapshot.active_scopes;
        ++impl_->snapshot.committed_reservations;
        if (block->live_allocations == 0u)
        {
            if (block->live_bytes != 0u)
                registryMemoryInvariantFailed();
            impl_->releaseBlock(block);
        }
        else
        {
            block->state = Impl::EBlockState::COMMITTED;
        }
    }

    void RegistryMemoryResource::reopenPublicationAdmission() noexcept
    {
        impl_->requireOwner();
        if (!impl_->publication_admission_closed)
            registryMemoryInvariantFailed();
        impl_->publication_admission_closed = false;
    }
} // namespace lux::meta
