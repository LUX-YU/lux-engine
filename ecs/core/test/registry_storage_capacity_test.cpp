#include <lux/engine/ecs/RegistryStorageCapacity.hpp>
#include <lux/engine/ecs/RegistryMemoryResource.hpp>

#include <entt/entt.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace
{
    struct AllocationCounts final
    {
        std::size_t calls{0u};
        std::size_t bytes{0u};
    };

    template <class Type>
    class CountingAllocator final
    {
    public:
        using value_type = Type;

        CountingAllocator()
            : counts_(std::make_shared<AllocationCounts>())
        {}

        explicit CountingAllocator(
            std::shared_ptr<AllocationCounts> counts) noexcept
            : counts_(std::move(counts))
        {}

        template <class Other>
        CountingAllocator(const CountingAllocator<Other>& other) noexcept
            : counts_(other.counts())
        {}

        [[nodiscard]] Type* allocate(std::size_t count)
        {
            ++counts_->calls;
            counts_->bytes += count * sizeof(Type);
            return std::allocator<Type>{}.allocate(count);
        }

        void deallocate(Type* pointer, std::size_t count) noexcept
        {
            std::allocator<Type>{}.deallocate(pointer, count);
        }

        [[nodiscard]] const std::shared_ptr<AllocationCounts>& counts()
            const noexcept
        {
            return counts_;
        }

        template <class Other>
        [[nodiscard]] bool operator==(
            const CountingAllocator<Other>& other) const noexcept
        {
            return counts_ == other.counts();
        }

    private:
        std::shared_ptr<AllocationCounts> counts_;
    };

    struct ProbeComponent final
    {
        std::uint32_t value{0u};
    };

    struct OtherProbeComponent final
    {
        std::uint64_t value{0u};
    };

    class CountingMemoryResource final
        : public lux::ecs::IRegistryMemoryUpstream
    {
    public:
        [[nodiscard]] std::size_t allocationCalls() const noexcept
        {
            return allocation_calls_;
        }

        [[nodiscard]] std::size_t liveBlocks() const noexcept
        {
            return live_blocks_;
        }

        [[nodiscard]] std::size_t liveBytes() const noexcept
        {
            return live_bytes_;
        }

        [[nodiscard]] void* tryAllocate(
            std::size_t bytes,
            std::size_t alignment) noexcept override
        {
            ++allocation_calls_;
            auto* result =
                lux::ecs::defaultRegistryMemoryUpstream().tryAllocate(
                bytes, alignment);
            if (result)
            {
                ++live_blocks_;
                live_bytes_ += bytes;
            }
            return result;
        }

        void deallocate(
            void* pointer,
            std::size_t bytes,
            std::size_t alignment) noexcept override
        {
            if (live_blocks_ == 0u || live_bytes_ < bytes)
                std::abort();
            --live_blocks_;
            live_bytes_ -= bytes;
            lux::ecs::defaultRegistryMemoryUpstream().deallocate(
                pointer, bytes, alignment);
        }

        std::size_t allocation_calls_{0u};
        std::size_t live_blocks_{0u};
        std::size_t live_bytes_{0u};
    };

    [[noreturn]] void fail(const char* message)
    {
        std::cerr << "registry_storage_capacity_test: " << message << '\n';
        std::exit(1);
    }

    void expect(bool condition, const char* message)
    {
        if (!condition)
            fail(message);
    }
}

int main()
{
    using Registry = entt::basic_registry<
        entt::entity,
        CountingAllocator<entt::entity>>;

    auto counts = std::make_shared<AllocationCounts>();
    Registry registry{CountingAllocator<entt::entity>{counts}};
    const auto low = registry.create();
    auto& storage = registry.storage<ProbeComponent>();

    const auto reserved =
        lux::ecs::reserveAdditionalStorageCapacity(storage, 1u);
    expect(reserved.has_value(), "low storage reservation failed");
    expect(
        reserved->preventsPackedGrowth(1u),
        "reservation did not cover one packed component");

    counts->calls = 0u;
    counts->bytes = 0u;
    registry.emplace<ProbeComponent>(low, ProbeComponent{7u});
    expect(
        counts->calls != 0u,
        "probe no longer demonstrates low-index sparse-page allocation");
    expect(
        storage.capacity() == reserved->capacity_after,
        "low-index sparse-page allocation changed packed component capacity");
    registry.remove<ProbeComponent>(low);

    // Even the low-index insertion above allocates the first sparse page:
    // storage::reserve() covers packed/payload capacity, not sparse entity-id
    // pages. Put a live entity on the next sparse page to prove the same
    // limitation is independent of packed/payload capacity.
    const auto high = registry.create(static_cast<entt::entity>(
        entt::entt_traits<entt::entity>::page_size));
    const auto packed_only =
        lux::ecs::reserveAdditionalStorageCapacity(storage, 1u);
    expect(packed_only.has_value(), "high storage reservation failed");

    counts->calls = 0u;
    counts->bytes = 0u;
    registry.emplace<ProbeComponent>(high, ProbeComponent{9u});
    expect(
        counts->calls != 0u,
        "probe no longer demonstrates EnTT sparse-page allocation");
    expect(
        storage.capacity() == packed_only->capacity_after,
        "sparse-page allocation changed packed component capacity");

    std::size_t target = 0u;
    expect(
        !lux::ecs::checkedAdditionalCapacity(
            std::numeric_limits<std::size_t>::max(), 1u, target),
        "capacity overflow was accepted");

    // RegistryMemoryResource is the allocator primitive below the canonical
    // EntityRegistry ABI. Both batches arm first; neither is allowed to
    // consume the other's private block.
    CountingMemoryResource upstream;
    auto resource = lux::ecs::RegistryMemoryResource::create(&upstream);
    using StrictRegistry = entt::basic_registry<
        entt::entity,
        lux::ecs::RegistryAllocator<entt::entity>>;
    StrictRegistry strict{
        lux::ecs::RegistryAllocator<entt::entity>{resource}};

    constexpr auto page_size =
        entt::entt_traits<entt::entity>::page_size;
    const auto first_high = strict.create(
        static_cast<entt::entity>(page_size + 7u));
    const auto recycled_source = strict.create(
        static_cast<entt::entity>(page_size * 3u + 11u));
    strict.destroy(recycled_source);
    const auto recycled_high = strict.create();
    expect(
        entt::entt_traits<entt::entity>::to_entity(recycled_high) ==
            entt::entt_traits<entt::entity>::to_entity(recycled_source),
        "destroyed high entity index was not recycled");

    auto& first_storage = strict.storage<ProbeComponent>();
    auto& second_storage = strict.storage<OtherProbeComponent>();
    expect(
        lux::ecs::reserveAdditionalStorageCapacity(first_storage, 1u)
            .has_value(),
        "strict first payload reservation failed");
    expect(
        lux::ecs::reserveAdditionalStorageCapacity(second_storage, 1u)
            .has_value(),
        "strict second payload reservation failed");

    const auto reservation_bytes =
        lux::ecs::registrySparsePublicationBytes(1u);
    expect(
        reservation_bytes.has_value(),
        "strict sparse-write budget overflowed");
    auto first_batch = resource->reservePublication(*reservation_bytes);
    auto second_batch = resource->reservePublication(*reservation_bytes);
    expect(first_batch.has_value(), "first strict reservation failed");
    expect(second_batch.has_value(), "second strict reservation failed");
    expect(
        resource->snapshot().armed_reservations == 2u,
        "two strict reservations were not independently armed");

    const auto calls_after_arm = upstream.allocationCalls();
    {
        auto scope = first_batch->enter();
        const auto nested = resource->reservePublication(
            *reservation_bytes);
        expect(
            !nested.has_value() &&
                nested.error() ==
                    lux::ecs::ERegistryPublicationReservationError::
                        PUBLICATION_ACTIVE,
            "active publication accepted a nested upstream reservation");
        strict.emplace<ProbeComponent>(first_high, ProbeComponent{17u});
    }
    expect(
        upstream.allocationCalls() == calls_after_arm,
        "cross-page publication reached the upstream resource");
    expect(
        resource->snapshot().armed_reservations == 1u,
        "first publication consumed another batch reservation");

    {
        auto scope = second_batch->enter();
        strict.emplace<OtherProbeComponent>(
            recycled_high, OtherProbeComponent{23u});
    }
    expect(
        upstream.allocationCalls() == calls_after_arm,
        "recycled high-id publication reached the upstream resource");
    const auto strict_snapshot = resource->snapshot();
    expect(
        strict_snapshot.armed_reservations == 0u &&
            strict_snapshot.active_scopes == 0u &&
            strict_snapshot.committed_reservations == 2u,
        "strict publication transaction did not settle");
    expect(
        strict_snapshot.publication_invariant_failures == 0u,
        "strict publication exhausted a reservation");

    // Stable sparse/payload capacity is the steady-state shape of repeated
    // Section publish/deactivate churn. Each cycle arms its own block, but a
    // publication which performs no new allocator request must release that
    // unused block immediately instead of retaining one historical arena per
    // generation.
    const auto steady_live_blocks = upstream.liveBlocks();
    const auto steady_live_bytes = upstream.liveBytes();
    for (std::size_t generation = 0u; generation < 256u; ++generation)
    {
        strict.remove<ProbeComponent>(first_high);
        auto churn = resource->reservePublication(*reservation_bytes);
        expect(churn.has_value(), "churn reservation failed");
        {
            auto scope = churn->enter();
            strict.emplace<ProbeComponent>(
                first_high,
                ProbeComponent{
                    static_cast<std::uint32_t>(generation)});
        }
        expect(
            upstream.liveBlocks() == steady_live_blocks &&
                upstream.liveBytes() == steady_live_bytes,
            "historical publication block survived stable-capacity churn");
    }
    const auto churn_snapshot = resource->snapshot();
    expect(
        churn_snapshot.live_upstream_blocks == upstream.liveBlocks() &&
            churn_snapshot.live_upstream_bytes == upstream.liveBytes() &&
            churn_snapshot.upstream_deallocation_calls >= 256u,
        "resource live-block accounting diverged during churn");

    // Ordinary allocator growth must retire the old bump block when the
    // container releases its last allocation; an empty reusable tail is not
    // allowed to become one permanent block per historical reallocation.
    CountingMemoryResource reallocation_upstream;
    auto reallocation_resource =
        lux::ecs::RegistryMemoryResource::create(&reallocation_upstream);
    {
        std::vector<
            std::uint64_t,
            lux::ecs::RegistryAllocator<std::uint64_t>> vector{
                lux::ecs::RegistryAllocator<std::uint64_t>{
                    reallocation_resource}};
        vector.reserve(4u);
        const auto after_first_reserve =
            reallocation_resource->snapshot();
        vector.reserve(4096u);
        const auto after_growth = reallocation_resource->snapshot();
        expect(
            after_growth.live_upstream_blocks == 1u &&
                after_growth.upstream_deallocation_calls ==
                    after_first_reserve.upstream_deallocation_calls + 1u,
            "normal vector growth retained its retired allocator block");
    }
    expect(
        reallocation_upstream.liveBlocks() == 0u &&
            reallocation_upstream.liveBytes() == 0u,
        "normal vector destruction retained its final allocator block");

    return 0;
}
