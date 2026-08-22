#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>

#include <iostream>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#if defined(_MSC_VER)
#  include <malloc.h>
#endif

static_assert(!std::is_copy_constructible_v<lux::ecs::Registry>);
static_assert(!std::is_copy_assignable_v<lux::ecs::Registry>);
static_assert(!std::is_move_constructible_v<lux::ecs::Registry>);
static_assert(!std::is_move_assignable_v<lux::ecs::Registry>);

namespace
{
    std::atomic<bool> g_allocation_gate_enabled{false};
    std::atomic<std::uint64_t> g_gated_new_calls{0u};

    void recordGlobalAllocation() noexcept
    {
        if (g_allocation_gate_enabled.load(std::memory_order_relaxed))
            g_gated_new_calls.fetch_add(1u, std::memory_order_relaxed);
    }

    [[nodiscard]] void* allocateGlobal(std::size_t bytes) noexcept
    {
        recordGlobalAllocation();
        return std::malloc(bytes == 0u ? 1u : bytes);
    }

    [[nodiscard]] void* allocateGlobalAligned(
        std::size_t bytes,
        std::size_t alignment) noexcept
    {
        recordGlobalAllocation();
#if defined(_MSC_VER)
        return _aligned_malloc(bytes == 0u ? 1u : bytes, alignment);
#else
        const auto requested = bytes == 0u ? 1u : bytes;
        const auto padded =
            (requested + alignment - 1u) / alignment * alignment;
        return std::aligned_alloc(alignment, padded);
#endif
    }
}

void* operator new(std::size_t bytes)
{
    if (auto* result = allocateGlobal(bytes))
        return result;
    std::abort();
}

void* operator new[](std::size_t bytes)
{
    return ::operator new(bytes);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    return allocateGlobal(bytes);
}

void* operator new[](
    std::size_t bytes,
    const std::nothrow_t&) noexcept
{
    return allocateGlobal(bytes);
}

void* operator new(std::size_t bytes, std::align_val_t alignment)
{
    if (auto* result = allocateGlobalAligned(
            bytes, static_cast<std::size_t>(alignment)))
    {
        return result;
    }
    std::abort();
}

void* operator new[](
    std::size_t bytes,
    std::align_val_t alignment)
{
    return ::operator new(bytes, alignment);
}

void* operator new(
    std::size_t bytes,
    std::align_val_t alignment,
    const std::nothrow_t&) noexcept
{
    return allocateGlobalAligned(
        bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](
    std::size_t bytes,
    std::align_val_t alignment,
    const std::nothrow_t&) noexcept
{
    return allocateGlobalAligned(
        bytes, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::align_val_t) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void operator delete[](void* pointer, std::align_val_t alignment) noexcept
{
    ::operator delete(pointer, alignment);
}

void operator delete(
    void* pointer,
    std::size_t,
    std::align_val_t alignment) noexcept
{
    ::operator delete(pointer, alignment);
}

void operator delete[](
    void* pointer,
    std::size_t,
    std::align_val_t alignment) noexcept
{
    ::operator delete(pointer, alignment);
}

namespace
{
    struct StagedService { int value{0}; };

    template <int Id>
    struct TopologyProbe final : lux::ecs::ISystem
    {
        std::vector<int>*                 order{nullptr};
        std::vector<int>*                 removal_order{nullptr};
        int*                              added{nullptr};
        std::vector<lux::ecs::SystemType> required;
        std::vector<lux::ecs::SystemType> after;
        std::vector<lux::ecs::SystemType> before;

        explicit TopologyProbe(
            std::vector<int>* order_sink = nullptr,
            int*              added_count = nullptr,
            std::vector<int>* removal_sink = nullptr
        ) noexcept
            : order(order_sink), removal_order(removal_sink), added(added_count)
        {
        }

        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            if (removal_order)
                removal_order->push_back(Id);
        }

        void onAdded(const lux::ecs::SystemSetupContext&) override
        {
            if (added) ++*added;
        }

        void update(const lux::ecs::SystemUpdateContext&) override
        {
            if (order) order->push_back(Id);
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        [[nodiscard]] std::span<const lux::ecs::SystemType>
        prerequisites() const noexcept override
        {
            return required;
        }

        [[nodiscard]] std::span<const lux::ecs::SystemType>
        runsAfter() const noexcept override
        {
            return after;
        }

        [[nodiscard]] std::span<const lux::ecs::SystemType>
        runsBefore() const noexcept override
        {
            return before;
        }

        [[nodiscard]] AccessDeclaration
        accessDeclaration() const noexcept override
        {
            return {.resources = {}, .complete = true, .structural = false};
        }
    };

    struct BatchResourceA {};
    struct BatchResourceB {};

    template <int Id, class Resource, bool Writes>
    struct AccessProbe final : lux::ecs::ISystem
    {
        void update(const lux::ecs::SystemUpdateContext&) override {}

        [[nodiscard]] AccessDeclaration
        accessDeclaration() const noexcept override
        {
            static constexpr ResourceAccess kAccess[] = {
                Writes ? writes<Resource>() : reads<Resource>()};
            return {
                .resources = kAccess,
                .complete = true,
                .structural = false,
            };
        }
    };

    struct ExclusiveProbe final : lux::ecs::ISystem
    {
        void update(const lux::ecs::SystemUpdateContext&) override {}
    };

    struct FrozenResource {};

    struct BarrierPublicationComponent final
    {
        std::uint32_t value{0u};
    };

    struct BarrierPublicationSystem;

    struct BarrierObservedCommand final
    {
        using Producer = BarrierPublicationSystem;

        [[nodiscard]] std::size_t registryPublicationBytes()
            const noexcept { return 0u; }
        void prepareRegistryPublication(
            lux::ecs::Registry&) const noexcept {}
        void apply(
            lux::ecs::Registry&,
            BarrierPublicationSystem&) const noexcept;
    };

    struct BarrierPublicationCommand final
    {
        using Producer = BarrierPublicationSystem;

        entt::entity entity{entt::null};
        entt::entity parent{entt::null};

        [[nodiscard]] std::size_t registryPublicationBytes()
            const noexcept
        {
            return lux::ecs::ecsCommandSparsePublicationBytes(4u);
        }
        void prepareRegistryPublication(
            lux::ecs::Registry& registry) const noexcept
        {
            lux::ecs::reserveEcsCommandStorage(
                registry.storage<BarrierPublicationComponent>(), 1u);
            lux::ecs::reserveEcsCommandStorage(
                registry.storage<lux::ecs::ParentComponent>(), 1u);
            auto& hierarchy = lux::ecs::ensureHierarchyIndex(registry);
            if (!hierarchy.reserveForAdditionalEdges(1u))
                std::abort();
        }

        void apply(
            lux::ecs::Registry& registry,
            BarrierPublicationSystem&) const noexcept
        {
            registry.emplace<BarrierPublicationComponent>(
                entity, BarrierPublicationComponent{73u});
            if (!lux::ecs::setParent(registry, entity, parent))
                std::abort();
        }
    };

    struct BarrierPublicationSystem final : lux::ecs::ISystem
    {
        entt::entity first_target{entt::null};
        entt::entity second_target{entt::null};
        entt::entity parent{entt::null};
        lux::ecs::EcsCommandWriter writer;
        entt::scoped_connection constructed;
        bool queued{false};
        std::uint32_t observer_enqueue_count{0u};
        std::uint32_t followups_applied{0u};

        BarrierPublicationSystem(
            entt::entity first_entity,
            entt::entity second_entity,
            entt::entity parent_entity) noexcept
            : first_target(first_entity),
              second_target(second_entity),
              parent(parent_entity)
        {}

        void onAdded(const lux::ecs::SystemSetupContext& setup) override
        {
            writer = setup.commands();
            constructed = setup.registry()
                .on_construct<BarrierPublicationComponent>()
                .connect<&BarrierPublicationSystem::onConstructed>(*this);
            queued = writer.push(
                    BarrierPublicationCommand{first_target, parent}) &&
                writer.push(
                    BarrierPublicationCommand{second_target, parent});
        }

        void onConstructed(
            lux::ecs::RegistryBase&,
            entt::entity) noexcept
        {
            if (writer.push(BarrierObservedCommand{}))
                ++observer_enqueue_count;
        }

        void update(const lux::ecs::SystemUpdateContext&) override {}
    };

    void BarrierObservedCommand::apply(
        lux::ecs::Registry&,
        BarrierPublicationSystem& producer) const noexcept
    {
        ++producer.followups_applied;
    }

    struct ZeroByteBarrierTriggerComponent final {};

    struct ZeroByteBarrierFollowupComponent final
    {
        std::uint32_t value{0u};
    };

    struct ZeroByteBarrierObserverSystem;

    struct ZeroByteDestroyCommand final
    {
        using Producer = ZeroByteBarrierObserverSystem;

        entt::entity entity{entt::null};

        [[nodiscard]] std::size_t registryPublicationBytes()
            const noexcept
        {
            return 0u;
        }

        void prepareRegistryPublication(
            lux::ecs::Registry&) const noexcept
        {}

        void apply(
            lux::ecs::Registry& registry,
            ZeroByteBarrierObserverSystem&) const noexcept
        {
            if (registry.valid(entity))
                registry.destroy(entity);
        }
    };

    struct ZeroByteObserverFollowupCommand final
    {
        using Producer = ZeroByteBarrierObserverSystem;

        entt::entity entity{entt::null};

        [[nodiscard]] std::size_t registryPublicationBytes()
            const noexcept
        {
            return lux::ecs::ecsCommandSparsePublicationBytes(1u);
        }

        void prepareRegistryPublication(
            lux::ecs::Registry& registry) const noexcept
        {
            lux::ecs::reserveEcsCommandStorage(
                registry.storage<ZeroByteBarrierFollowupComponent>(), 1u);
        }

        void apply(
            lux::ecs::Registry& registry,
            ZeroByteBarrierObserverSystem& producer) const noexcept;
    };

    struct ZeroByteBarrierObserverSystem final : lux::ecs::ISystem
    {
        entt::entity doomed{entt::null};
        entt::entity followup_target{entt::null};
        lux::ecs::EcsCommandWriter writer;
        entt::scoped_connection destroyed;
        bool queued{false};
        std::uint32_t observer_enqueue_count{0u};
        std::uint32_t followups_applied{0u};

        ZeroByteBarrierObserverSystem(
            entt::entity doomed_entity,
            entt::entity target_entity) noexcept
            : doomed(doomed_entity), followup_target(target_entity)
        {}

        void onAdded(const lux::ecs::SystemSetupContext& setup) override
        {
            writer = setup.commands();
            destroyed = setup.registry()
                .on_destroy<ZeroByteBarrierTriggerComponent>()
                .connect<
                    &ZeroByteBarrierObserverSystem::onTriggerDestroyed>(*this);
            queued = writer.push(ZeroByteDestroyCommand{doomed}).has_value();
        }

        void onTriggerDestroyed(
            lux::ecs::RegistryBase&,
            entt::entity) noexcept
        {
            if (writer.push(
                    ZeroByteObserverFollowupCommand{followup_target}))
            {
                ++observer_enqueue_count;
            }
        }

        void update(const lux::ecs::SystemUpdateContext&) override {}
    };

    void ZeroByteObserverFollowupCommand::apply(
        lux::ecs::Registry& registry,
        ZeroByteBarrierObserverSystem& producer) const noexcept
    {
        if (registry.valid(entity) &&
            !registry.all_of<ZeroByteBarrierFollowupComponent>(entity))
        {
            registry.emplace<ZeroByteBarrierFollowupComponent>(
                entity, ZeroByteBarrierFollowupComponent{91u});
        }
        ++producer.followups_applied;
    }

    struct FrozenDescriptorTarget final : lux::ecs::ISystem
    {
        std::vector<lux::ecs::SystemType> after;
        std::vector<std::string_view>     features{"frozen.feature"};
        bool                              write_access{false};
        bool                              removable{false};

        void update(const lux::ecs::SystemUpdateContext&) override {}

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return removable;
        }

        [[nodiscard]] std::span<const lux::ecs::SystemType>
        runsAfter() const noexcept override
        {
            return after;
        }

        [[nodiscard]] AccessDeclaration
        accessDeclaration() const noexcept override
        {
            static constexpr ResourceAccess kRead[] = {
                reads<FrozenResource>()};
            static constexpr ResourceAccess kWrite[] = {
                writes<FrozenResource>()};
            return {
                .resources = write_access
                    ? std::span<const ResourceAccess>{kWrite}
                    : std::span<const ResourceAccess>{kRead},
                .complete = true,
                .structural = false,
            };
        }

    };

    struct FrozenDescriptorPeer final : lux::ecs::ISystem
    {
        void update(const lux::ecs::SystemUpdateContext&) override {}

        [[nodiscard]] AccessDeclaration
        accessDeclaration() const noexcept override
        {
            static constexpr ResourceAccess kRead[] = {
                reads<FrozenResource>()};
            return {
                .resources = kRead,
                .complete = true,
                .structural = false,
            };
        }
    };

    struct FrozenDescriptorMutator final : lux::ecs::ISystem
    {
        FrozenDescriptorTarget* target{nullptr};

        explicit FrozenDescriptorMutator(
            FrozenDescriptorTarget& target_system
        ) noexcept
            : target(&target_system)
        {
        }

        void onAdded(const lux::ecs::SystemSetupContext&) override
        {
            target->after = {
                lux::ecs::systemType<FrozenDescriptorTarget>()};
            target->features = {"mutated.feature"};
            target->write_access = true;
            target->removable = true;
        }

        void update(const lux::ecs::SystemUpdateContext&) override {}

        [[nodiscard]] AccessDeclaration
        accessDeclaration() const noexcept override
        {
            return {.resources = {}, .complete = true, .structural = false};
        }
    };

    struct ReentrantProbe;

    struct MarkReentrantUpdate final
    {
        using Producer = ReentrantProbe;

        int* applied{nullptr};
        [[nodiscard]] std::size_t registryPublicationBytes()
            const noexcept { return 0u; }
        void prepareRegistryPublication(
            lux::ecs::Registry&) const noexcept {}
        void apply(
            lux::ecs::Registry&,
            ReentrantProbe&
        ) const noexcept;
    };

    struct ReentrantProbe final : lux::ecs::ISystem
    {
        lux::ecs::Schedule* schedule{nullptr};
        lux::ecs::ScheduleBuilder* builder{nullptr};
        lux::ecs::SystemHandle<TopologyProbe<30>> removable;
        lux::ecs::EcsCommandWriter writer;
        int* update_count{nullptr};
        int* applied_count{nullptr};
        bool lifecycle_compile_rejected{false};
        bool lifecycle_batches_rejected{false};
        bool lifecycle_add_rejected{false};
        bool lifecycle_remove_rejected{false};
        bool lifecycle_builder_add_rejected{false};
        bool lifecycle_builder_commit_rejected{false};
        bool update_command_queued{false};

        ReentrantProbe(
            lux::ecs::Schedule& schedule_owner,
            lux::ecs::ScheduleBuilder& assembly,
            lux::ecs::SystemHandle<TopologyProbe<30>> removable_system,
            int& updates,
            int& applied
        ) noexcept
            : schedule(&schedule_owner),
              builder(&assembly),
              removable(removable_system),
              update_count(&updates),
              applied_count(&applied)
        {
        }

        void onAdded(const lux::ecs::SystemSetupContext& setup) override
        {
            writer = setup.commands();
            const auto report = schedule->compile();
            lifecycle_compile_rejected = report.operation_rejected &&
                                         !report.valid();
            schedule->tick(0.0f);
            schedule->applyCommandBarrier();
            lifecycle_batches_rejected =
                schedule->executionBatches().empty();

            const auto added = schedule->addSystem(
                std::make_unique<TopologyProbe<31>>()
            );
            lifecycle_add_rejected = !added && added.error() ==
                lux::ecs::EScheduleMutationError::ReentrantMutation;

            const auto removed = schedule->removeSystem(removable);
            lifecycle_remove_rejected = !removed && removed.error() ==
                lux::ecs::EScheduleMutationError::ReentrantMutation;

            const auto staged = builder->add(
                std::make_unique<TopologyProbe<32>>()
            );
            lifecycle_builder_add_rejected = !staged && staged.error() ==
                lux::ecs::EScheduleBuildError::CommitInProgress;

            const auto recommitted = builder->commit();
            lifecycle_builder_commit_rejected = !recommitted &&
                recommitted.error().error ==
                    lux::ecs::EScheduleCommitError::ScheduleBusy;
        }

        void update(const lux::ecs::SystemUpdateContext&) override
        {
            ++*update_count;
            update_command_queued = writer.push(
                MarkReentrantUpdate{applied_count}
            ).has_value();

            (void)schedule->compile();
            schedule->tick(0.0f);
            schedule->applyCommandBarrier();
            (void)schedule->executionBatches();
        }

        [[nodiscard]] AccessDeclaration
        accessDeclaration() const noexcept override
        {
            return {.resources = {}, .complete = true, .structural = false};
        }
    };

    void MarkReentrantUpdate::apply(
        lux::ecs::Registry&,
        ReentrantProbe&
    ) const noexcept
    {
        if (applied) ++*applied;
    }

    [[nodiscard]] bool expect(bool condition, const char* message)
    {
        if (condition) return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    [[nodiscard]] bool containsSystemType(
        const std::vector<lux::ecs::SystemType>& types,
        lux::ecs::SystemType                    expected) noexcept
    {
        for (const auto type : types)
            if (lux::ecs::sameSystemType(type, expected))
                return true;
        return false;
    }

    [[nodiscard]] bool testPendingCycleAndStructuredFailure()
    {
        int added = 0;
        lux::ecs::SceneServices services;
        lux::ecs::World         world;
        lux::ecs::Schedule      schedule{world};
        lux::ecs::ScheduleBuilder builder{schedule, services};

        auto one = std::make_unique<TopologyProbe<1>>(nullptr, &added);
        auto two = std::make_unique<TopologyProbe<2>>(nullptr, &added);
        auto downstream =
            std::make_unique<TopologyProbe<3>>(nullptr, &added);
        one->after = {lux::ecs::systemType<TopologyProbe<2>>()};
        two->after = {lux::ecs::systemType<TopologyProbe<1>>()};
        downstream->after = {
            lux::ecs::systemType<TopologyProbe<1>>()};

        if (!expect(
                builder.add(std::move(one)).has_value() &&
                    builder.add(std::move(two)).has_value() &&
                    builder.add(std::move(downstream)).has_value() &&
                    builder.services().emplace<StagedService>().has_value(),
                "pending cycle fixture stages systems and service"))
            return false;

        const auto committed = builder.commit();
        if (!expect(
                !committed &&
                    committed.error().error ==
                        lux::ecs::EScheduleCommitError::TopologyCycle,
                "pending-only cycle is rejected"))
            return false;

        const auto& topology = committed.error().topology;
        if (!expect(
                topology.cycle.size() == 2 &&
                    containsSystemType(
                        topology.cycle,
                        lux::ecs::systemType<TopologyProbe<1>>()) &&
                    containsSystemType(
                        topology.cycle,
                        lux::ecs::systemType<TopologyProbe<2>>()) &&
                    !containsSystemType(
                        topology.cycle,
                        lux::ecs::systemType<TopologyProbe<3>>()),
                "structured failure reports the exact SCC without downstream"))
            return false;

        return expect(
            schedule.systemCount() == 0 && added == 0 &&
                !services.contains<StagedService>() &&
                builder.services().contains<StagedService>(),
            "cycle rejection runs no onAdded and publishes no staged service"
        );
    }

    [[nodiscard]] bool testSelfCycle()
    {
        int added = 0;
        lux::ecs::SceneServices services;
        lux::ecs::World         world;
        lux::ecs::Schedule      schedule{world};
        lux::ecs::ScheduleBuilder builder{schedule, services};

        auto self = std::make_unique<TopologyProbe<4>>(nullptr, &added);
        self->after = {lux::ecs::systemType<TopologyProbe<4>>()};
        if (!expect(builder.add(std::move(self)).has_value(),
                    "self-cycle fixture stages"))
            return false;

        const auto committed = builder.commit();
        return expect(
            !committed &&
                committed.error().error ==
                    lux::ecs::EScheduleCommitError::TopologyCycle &&
                committed.error().topology.cycle.size() == 1 &&
                lux::ecs::sameSystemType(
                    committed.error().topology.cycle.front(),
                    lux::ecs::systemType<TopologyProbe<4>>()) &&
                schedule.systemCount() == 0 && added == 0,
            "self-edge is a one-node SCC rejected before onAdded"
        );
    }

    [[nodiscard]] bool testLivePendingCycleIsAtomic()
    {
        int live_added = 0;
        int pending_added = 0;
        lux::ecs::SceneServices services;
        lux::ecs::World         world;
        lux::ecs::Schedule      schedule{world};

        auto live_system =
            std::make_unique<TopologyProbe<5>>(nullptr, &live_added);
        live_system->after = {
            lux::ecs::systemType<TopologyProbe<6>>()};
        const auto live = schedule.addSystem(std::move(live_system));
        if (!expect(live && live_added == 1,
                    "live half accepts its unresolved optional edge"))
            return false;

        auto* const live_instance = schedule.get(*live);
        const auto live_slot = live->slot();
        const auto live_generation = live->generation();

        lux::ecs::ScheduleBuilder builder{schedule, services};
        auto pending =
            std::make_unique<TopologyProbe<6>>(nullptr, &pending_added);
        pending->after = {lux::ecs::systemType<TopologyProbe<5>>()};
        auto pending_ref = builder.add(std::move(pending));
        if (!expect(pending_ref.has_value(),
                    "pending half of live-plus-pending cycle stages"))
            return false;

        const auto committed = builder.commit();
        if (!expect(
                !committed &&
                    committed.error().error ==
                        lux::ecs::EScheduleCommitError::TopologyCycle &&
                    committed.error().topology.cycle.size() == 2,
                "live-plus-pending cycle is rejected"))
            return false;

        return expect(
            schedule.systemCount() == 1 && pending_added == 0 &&
                !builder.handle(*pending_ref).valid() &&
                live->slot() == live_slot &&
                live->generation() == live_generation &&
                schedule.get(*live) == live_instance,
            "failed commit preserves the live slot and generation"
        );
    }

    [[nodiscard]] bool testUnknownEdges()
    {
        {
            lux::ecs::World    world;
            lux::ecs::Schedule schedule{world};

            constexpr auto target_type =
                lux::ecs::systemType<TopologyProbe<7>>();
            constexpr lux::ecs::SystemType collision{
                target_type.hash(),
                "collision.guard.not.the.real.type",
            };

            auto source = std::make_unique<TopologyProbe<8>>();
            source->after = {collision};
            const auto target_added = schedule.addSystem(
                std::make_unique<TopologyProbe<7>>());
            const auto source_added = schedule.addSystem(std::move(source));
            if (!expect(target_added && source_added,
                        "hash-collision fixture installs"))
                return false;

            const auto report = schedule.compile();
            if (!expect(
                    report.valid() && report.unknown.size() == 1 &&
                        lux::ecs::sameSystemType(
                            report.unknown.front(), collision),
                    "equal hash with a different name remains unknown"))
                return false;
        }

        {
            lux::ecs::SceneServices services;
            lux::ecs::World         world;
            lux::ecs::Schedule      schedule{world};
            lux::ecs::ScheduleBuilder builder{schedule, services};

            auto optional = std::make_unique<TopologyProbe<9>>();
            optional->after = {
                lux::ecs::systemType<TopologyProbe<99>>()};
            if (!expect(builder.add(std::move(optional)).has_value() &&
                            builder.commit().has_value(),
                        "unknown optional ordering edge is non-fatal"))
                return false;

            const auto report = schedule.compile();
            if (!expect(
                    report.valid() && report.unknown.size() == 1 &&
                        lux::ecs::sameSystemType(
                            report.unknown.front(),
                            lux::ecs::systemType<TopologyProbe<99>>()),
                    "compile retains the accepted unknown edge"))
                return false;
        }

        return true;
    }

    [[nodiscard]] bool testStableSequenceAndBatchGolden()
    {
        {
            std::vector<int> order;
            lux::ecs::World    world;
            lux::ecs::Schedule schedule{world};
            const auto retired = schedule.addSystem(
                std::make_unique<TopologyProbe<10>>(&order));
            const auto incumbent = schedule.addSystem(
                std::make_unique<TopologyProbe<11>>(&order));
            if (!expect(
                    retired && incumbent &&
                        schedule.removeSystem(*retired).has_value(),
                    "slot-reuse fixture retires its first node"))
                return false;
            const auto replacement = schedule.addSystem(
                std::make_unique<TopologyProbe<12>>(&order));
            if (!expect(
                    replacement.has_value() &&
                        replacement->slot() == retired->slot() &&
                        replacement->generation() != retired->generation(),
                    "slot reuse advances generation for the replacement"))
                return false;

            const auto report = schedule.compile();
            schedule.tick(0.0f);
            if (!expect(
                    report.valid() &&
                        order == std::vector<int>({11, 12}),
                    "stable sequence does not regress to recycled slot order"))
                return false;
        }

        {
            lux::ecs::World    world;
            lux::ecs::Schedule schedule{world};
            const auto read_a_1 = schedule.addSystem(
                std::make_unique<AccessProbe<1, BatchResourceA, false>>());
            const auto read_a_2 = schedule.addSystem(
                std::make_unique<AccessProbe<2, BatchResourceA, false>>());
            const auto write_a = schedule.addSystem(
                std::make_unique<AccessProbe<3, BatchResourceA, true>>());
            const auto write_b = schedule.addSystem(
                std::make_unique<AccessProbe<4, BatchResourceB, true>>());
            const auto exclusive = schedule.addSystem(
                std::make_unique<ExclusiveProbe>());
            if (!expect(
                    read_a_1 && read_a_2 && write_a && write_b && exclusive,
                    "batch golden fixture installs"))
                return false;

            const auto report = schedule.compile();
            const auto batches = schedule.executionBatches();
            if (!expect(
                    report.valid() && batches.size() == 3 &&
                        batches[0].first == 0 && batches[0].count == 2 &&
                        batches[1].first == 2 && batches[1].count == 2 &&
                        batches[2].first == 4 && batches[2].count == 1,
                    "access declarations preserve the execution-batch golden"))
                return false;
        }

        return true;
    }

    [[nodiscard]] bool testDirectMutationPreflight()
    {
        {
            int added = 0;
            lux::ecs::World    world;
            lux::ecs::Schedule schedule{world};

            auto missing =
                std::make_unique<TopologyProbe<13>>(nullptr, &added);
            missing->required = {
                lux::ecs::systemType<TopologyProbe<14>>()};
            const auto result = schedule.addSystem(std::move(missing));
            if (!expect(
                    !result &&
                        result.error() == lux::ecs::
                            EScheduleMutationError::MissingPrerequisite &&
                        schedule.systemCount() == 0 && added == 0,
                    "direct add rejects missing prerequisite before onAdded"))
                return false;
        }

        {
            int added = 0;
            lux::ecs::World    world;
            lux::ecs::Schedule schedule{world};

            auto live_system =
                std::make_unique<TopologyProbe<15>>(nullptr, &added);
            live_system->after = {
                lux::ecs::systemType<TopologyProbe<16>>()};
            const auto live = schedule.addSystem(std::move(live_system));
            if (!expect(live && added == 1,
                        "direct-cycle live half installs"))
                return false;

            auto candidate =
                std::make_unique<TopologyProbe<16>>(nullptr, &added);
            candidate->after = {
                lux::ecs::systemType<TopologyProbe<15>>()};
            const auto result = schedule.addSystem(std::move(candidate));
            if (!expect(
                    !result &&
                        result.error() == lux::ecs::
                            EScheduleMutationError::TopologyCycle &&
                        schedule.systemCount() == 1 && added == 1 &&
                        schedule.get(*live) != nullptr,
                    "direct add rejects cycle before ownership and onAdded"))
                return false;
        }

        return true;
    }

    [[nodiscard]] bool testRemovalPrerequisiteGuard()
    {
        lux::ecs::World    world;
        lux::ecs::Schedule schedule{world};
        const auto provider = schedule.addSystem(
            std::make_unique<TopologyProbe<17>>());
        if (!expect(provider.has_value(),
                    "prerequisite provider installs"))
            return false;

        auto dependent_system = std::make_unique<TopologyProbe<18>>();
        dependent_system->required = {
            lux::ecs::systemType<TopologyProbe<17>>()};
        const auto dependent =
            schedule.addSystem(std::move(dependent_system));
        if (!expect(dependent.has_value(),
                    "hard-prerequisite dependent installs"))
            return false;

        const auto refused = schedule.removeSystem(*provider);
        if (!expect(
                !refused &&
                    refused.error() == lux::ecs::
                        EScheduleMutationError::RequiredByOtherSystem &&
                    schedule.get(*provider) != nullptr &&
                    schedule.get(*dependent) != nullptr,
                "removal cannot strand a hard prerequisite"))
            return false;

        return expect(
            schedule.removeSystem(*dependent).has_value() &&
                schedule.removeSystem(*provider).has_value(),
            "dependency-safe reverse removal succeeds"
        );
    }

    [[nodiscard]] bool testStructuredMissingPrerequisite()
    {
        int added = 0;
        lux::ecs::SceneServices services;
        lux::ecs::World         world;
        lux::ecs::Schedule      schedule{world};
        lux::ecs::ScheduleBuilder builder{schedule, services};

        auto subject =
            std::make_unique<TopologyProbe<19>>(nullptr, &added);
        subject->required = {
            lux::ecs::systemType<TopologyProbe<20>>()};
        if (!expect(builder.add(std::move(subject)).has_value(),
                    "missing-prerequisite fixture stages"))
            return false;

        const auto committed = builder.commit();
        if (!expect(
                !committed &&
                    committed.error().error == lux::ecs::
                        EScheduleCommitError::MissingPrerequisite &&
                    committed.error().topology.missing_prereq.size() == 1,
                "missing prerequisite returns structured topology evidence"))
            return false;

        const auto& [reported_subject, reported_required] =
            committed.error().topology.missing_prereq.front();
        return expect(
            lux::ecs::sameSystemType(
                reported_subject,
                lux::ecs::systemType<TopologyProbe<19>>()) &&
                lux::ecs::sameSystemType(
                    reported_required,
                    lux::ecs::systemType<TopologyProbe<20>>()) &&
                schedule.systemCount() == 0 && added == 0,
            "structured missing pair preserves subject and prerequisite"
        );
    }

    [[nodiscard]] bool testDescriptorCaptureBoundary()
    {
        // add() only transfers ownership into the unpublished builder. Packs
        // may still resolve descriptor inputs before commit; the freeze must
        // therefore happen at commit, not at add().
        {
            std::vector<int> order;
            lux::ecs::SceneServices services;
            lux::ecs::World         world;
            lux::ecs::Schedule      schedule{world};
            lux::ecs::ScheduleBuilder builder{schedule, services};

            auto pending = builder.add(
                std::make_unique<TopologyProbe<21>>(&order)
            );
            if (!expect(pending.has_value(),
                        "descriptor-boundary target stages"))
                return false;

            lux::ecs::SceneServices foreign_services;
            lux::ecs::ScheduleBuilder foreign_builder{
                schedule,
                foreign_services
            };
            if (!expect(
                    foreign_builder.add(
                        std::make_unique<TopologyProbe<21>>()
                    ).has_value() &&
                        foreign_builder.get(*pending) == nullptr,
                    "pending token cannot resolve through another builder"))
                return false;

            builder.get(*pending)->after = {
                lux::ecs::systemType<TopologyProbe<22>>()};
            if (!expect(
                    builder.add(
                        std::make_unique<TopologyProbe<22>>(&order)
                    ).has_value() &&
                        builder.commit().has_value(),
                    "descriptor edits before commit are accepted"))
                return false;

            const auto report = schedule.compile();
            schedule.tick(0.0f);
            if (!expect(
                    report.valid() &&
                        order == std::vector<int>({22, 21}),
                    "commit captures the final unpublished descriptor"))
                return false;
        }

        // Every descriptor in the batch is captured before the first onAdded.
        // A callback may mutate another system's runtime members, but it cannot
        // rewrite the graph that was preflighted and published.
        {
            lux::ecs::SceneServices services;
            lux::ecs::World         world;
            lux::ecs::Schedule      schedule{world};
            lux::ecs::ScheduleBuilder builder{schedule, services};

            auto target = std::make_unique<FrozenDescriptorTarget>();
            auto* const target_instance = target.get();
            if (!expect(
                    builder.add(
                        std::make_unique<FrozenDescriptorMutator>(*target)
                    ).has_value(),
                    "descriptor mutator stages before its target"))
                return false;

            auto pending_target = builder.add(std::move(target));
            if (!expect(
                    pending_target.has_value() &&
                        builder.add(
                            std::make_unique<FrozenDescriptorPeer>()
                        ).has_value() &&
                        builder.commit().has_value(),
                    "frozen descriptor batch commits"))
                return false;

            const auto report = schedule.compile();
            const auto batches = schedule.executionBatches();
            if (!expect(
                    target_instance->write_access &&
                        target_instance->removable &&
                        target_instance->after.size() == 1 &&
                        report.valid() && batches.size() == 1 &&
                        batches.front().count == 3,
                    "onAdded mutations cannot alter frozen topology, access, "
                    "or removal policy"))
                return false;

            const auto removal = schedule.removeSystem(
                builder.handle(*pending_target)
            );
            if (!expect(
                    !removal && removal.error() == lux::ecs::
                        EScheduleMutationError::RemovalUnsupported,
                    "removal capability comes from the frozen descriptor"))
                return false;
        }

        return true;
    }

    [[nodiscard]] bool testServicePublishConflictIsAtomic()
    {
        int added = 0;
        lux::ecs::SceneServices services;
        lux::ecs::World         world;
        lux::ecs::Schedule      schedule{world};
        lux::ecs::ScheduleBuilder builder{schedule, services};

        const auto staged = builder.services().emplace<StagedService>();
        auto pending = builder.add(
            std::make_unique<TopologyProbe<23>>(nullptr, &added)
        );
        const bool edit_queued =
            builder.services().deferStagedEdit<StagedService>(
                [](StagedService& service) noexcept { service.value = 1; }
            );
        const auto live = services.emplace<StagedService>();
        if (!expect(staged && pending && edit_queued && live,
                    "service-conflict fixture stages both owners"))
            return false;

        const auto committed = builder.commit();
        return expect(
            !committed && committed.error().error ==
                lux::ecs::EScheduleCommitError::ServiceConflict &&
                committed.error().detail ==
                    lux::ecs::sceneServiceType<StagedService>().name() &&
                schedule.systemCount() == 0 && added == 0 &&
                !builder.handle(*pending).valid() &&
                services.get<StagedService>() == *live &&
                builder.services().get<StagedService>() == *staged &&
                (**staged).value == 0,
            "service conflict publishes nothing, applies no edit and retains "
            "staged ownership"
        );
    }

    [[nodiscard]] bool testReentrantOperationsAreRejected()
    {
        int updates = 0;
        int applied = 0;
        lux::ecs::SceneServices services;
        lux::ecs::World         world;
        lux::ecs::Schedule      schedule{world};

        const auto removable = schedule.addSystem(
            std::make_unique<TopologyProbe<30>>()
        );
        if (!expect(removable.has_value(),
                    "reentrant fixture installs removable anchor"))
            return false;

        lux::ecs::ScheduleBuilder builder{schedule, services};
        auto pending = builder.add(std::make_unique<ReentrantProbe>(
            schedule,
            builder,
            *removable,
            updates,
            applied
        ));
        if (!expect(pending && builder.commit().has_value(),
                    "reentrant lifecycle probe commits"))
            return false;

        const auto probe_handle = builder.handle(*pending);
        const auto* probe = schedule.get(probe_handle);
        if (!expect(
                probe->lifecycle_compile_rejected &&
                    probe->lifecycle_batches_rejected &&
                    probe->lifecycle_add_rejected &&
                    probe->lifecycle_remove_rejected &&
                    probe->lifecycle_builder_add_rejected &&
                    probe->lifecycle_builder_commit_rejected &&
                    schedule.rejectedOperationCount() == 6 &&
                    schedule.tickIndex() == 0 &&
                    updates == 0 && applied == 0 &&
                    schedule.get(*removable) != nullptr &&
                    schedule.systemCount() == 2,
                "activation reentry is visible and has zero side effects"))
            return false;

        const auto report = schedule.compile();
        schedule.tick(0.0f);
        return expect(
            report.valid() && updates == 1 && applied == 1 &&
                probe->update_command_queued &&
                schedule.tickIndex() == 1 &&
                schedule.rejectedOperationCount() == 10,
            "update reentry is rejected while the outer tick and barrier finish"
        );
    }

    [[nodiscard]] bool testDynamicBatchMutation()
    {
        lux::ecs::World world;
        lux::ecs::Schedule schedule{world};
        std::vector<int> update_order;
        std::vector<int> removal_order;
        int added = 0;

        auto first = std::make_unique<TopologyProbe<40>>(
            &update_order,
            &added,
            &removal_order);
        auto second = std::make_unique<TopologyProbe<41>>(
            &update_order,
            &added,
            &removal_order);
        second->required = {lux::ecs::systemType<TopologyProbe<40>>()};
        second->after = {lux::ecs::systemType<TopologyProbe<40>>()};

        lux::ecs::ScheduleMutationBatch mutation;
        const auto staged_first = mutation.add(std::move(first));
        const auto staged_second = mutation.add(std::move(second));
        auto installed = schedule.installBatch(std::move(mutation));
        if (!expect(
                staged_first && staged_second && installed && added == 2 &&
                    schedule.systemCount() == 2,
                "dynamic system batch installs atomically"))
            return false;

        schedule.tick(0.0f);
        if (!expect(
                update_order == std::vector<int>({40, 41}),
                "dynamic batch uses the compiled topology order"))
            return false;

        auto dependent = std::make_unique<TopologyProbe<42>>();
        dependent->required = {lux::ecs::systemType<TopologyProbe<41>>()};
        const auto dependent_handle = schedule.addSystem(std::move(dependent));
        if (!expect(dependent_handle.has_value(),
                    "external dependent installs for removal guard"))
            return false;

        auto refused = schedule.removeBatch(std::move(*installed));
        if (!expect(
                !refused && refused.error().code ==
                    lux::ecs::EScheduleBatchError::REQUIRED_BY_OTHER_SYSTEM &&
                    schedule.systemCount() == 3,
                "batch removal rejects a live external dependent"))
            return false;

        if (!expect(schedule.removeSystem(*dependent_handle).has_value(),
                    "external dependent can be removed first"))
            return false;
        if (!expect(schedule.removeBatch(std::move(*installed)).has_value(),
                    "batch removes atomically after dependents are gone"))
            return false;
        if (!expect(
                schedule.systemCount() == 0 &&
                    removal_order == std::vector<int>({41, 40}),
                "batch removal invokes lifecycle in reverse execution order"))
            return false;

        auto cycle_a = std::make_unique<TopologyProbe<43>>();
        auto cycle_b = std::make_unique<TopologyProbe<44>>();
        cycle_a->after = {lux::ecs::systemType<TopologyProbe<44>>()};
        cycle_b->after = {lux::ecs::systemType<TopologyProbe<43>>()};
        lux::ecs::ScheduleMutationBatch invalid;
        (void)invalid.add(std::move(cycle_a));
        (void)invalid.add(std::move(cycle_b));
        const auto rejected = schedule.installBatch(std::move(invalid));
        return expect(
            !rejected && rejected.error().code ==
                lux::ecs::EScheduleBatchError::TOPOLOGY_CYCLE &&
                schedule.systemCount() == 0,
            "invalid dynamic batch has zero side effects");
    }

    [[nodiscard]] bool testSystemFrameTrace()
    {
        lux::ecs::World world;
        lux::ecs::Schedule schedule{world};
        std::vector<int> order;
        if (!schedule.addSystem(
                std::make_unique<TopologyProbe<50>>(&order),
                lux::ecs::kPhaseInput) ||
            !schedule.addSystem(
                std::make_unique<TopologyProbe<51>>(&order),
                lux::ecs::kPhaseSimulation))
        {
            return expect(false, "trace fixture installs systems");
        }
        const auto report = schedule.compile();
        const auto compiled = schedule.latestSystemFrameTrace();
        if (!expect(
                report.valid() && compiled.size() == 2u &&
                    compiled[0].frame_serial == 0u &&
                    lux::ecs::sameSystemType(
                        compiled[0].system,
                        lux::ecs::systemType<TopologyProbe<50>>()) &&
                    lux::ecs::sameSystemType(
                        compiled[1].system,
                        lux::ecs::systemType<TopologyProbe<51>>()),
                "compile preallocates trace in topology order"))
        {
            return false;
        }

        schedule.tick(0.0f, lux::ecs::kPhaseLast, 4242u);
        const auto trace = schedule.latestSystemFrameTrace();
        const auto& frame = schedule.latestFrameTrace();
        return expect(
            trace.size() == 2u &&
                trace[0].frame_serial == 4242u &&
                trace[1].frame_serial == 4242u &&
                frame.frame_serial == 4242u &&
                frame.phase_nanoseconds[0] == trace[0].wall_nanoseconds &&
                frame.phase_nanoseconds[2] == trace[1].wall_nanoseconds,
            "tick fills bounded system trace and exact phase sums");
    }

    [[nodiscard]] bool testCommandBarrierPublicationReservation()
    {
        lux::ecs::World world;
        auto& registry = world.registry();
        constexpr auto page_size =
            entt::entt_traits<entt::entity>::page_size;
        const auto first_target = registry.create(
            static_cast<entt::entity>(page_size * 3u + 19u));
        const auto second_target = registry.create(
            static_cast<entt::entity>(page_size * 5u + 23u));
        const auto parent = registry.create();

        lux::ecs::Schedule schedule{world};
        auto added = schedule.addSystem(
            std::make_unique<BarrierPublicationSystem>(
                first_target, second_target, parent));
        if (!expect(added.has_value(), "barrier fixture installs producer"))
            return false;
        auto* producer = schedule.get(*added);
        const auto armed = registry.memorySnapshot();
        if (!expect(
                producer && producer->queued &&
                    armed.armed_reservations == 2u &&
                    schedule.prepareCommandBarrier(),
                "two command publications arm and preflight cumulatively"))
        {
            return false;
        }

        const auto prepared = registry.memorySnapshot();

        const auto calls_after_arm = prepared.upstream_allocation_calls;
        const auto global_calls_before =
            g_gated_new_calls.load(std::memory_order_relaxed);
        g_allocation_gate_enabled.store(true, std::memory_order_relaxed);
        schedule.applyCommandBarrier();
        g_allocation_gate_enabled.store(false, std::memory_order_relaxed);
        const auto published = registry.memorySnapshot();
        if (!expect(
            registry.all_of<BarrierPublicationComponent>(first_target) &&
                registry.all_of<BarrierPublicationComponent>(second_target) &&
                registry.get<lux::ecs::ParentComponent>(first_target).parent() ==
                    parent &&
                registry.get<lux::ecs::ParentComponent>(second_target).parent() ==
                    parent &&
                registry.get<BarrierPublicationComponent>(first_target).value ==
                    73u &&
                registry.get<BarrierPublicationComponent>(second_target).value ==
                    73u &&
                published.upstream_allocation_calls == calls_after_arm &&
                published.armed_reservations == 0u &&
                published.active_scopes == 0u &&
                published.committed_reservations ==
                    armed.committed_reservations + 2u &&
                published.publication_invariant_failures == 0u &&
                g_gated_new_calls.load(std::memory_order_relaxed) ==
                    global_calls_before &&
                producer->observer_enqueue_count == 2u &&
                producer->followups_applied == 0u,
            "full barrier publishes two ARMED commands and defers signals"))
        {
            return false;
        }

        const auto second_global_calls_before =
            g_gated_new_calls.load(std::memory_order_relaxed);
        if (!schedule.prepareCommandBarrier())
            return expect(false, "signal follow-up barrier preflights");
        g_allocation_gate_enabled.store(true, std::memory_order_relaxed);
        schedule.applyCommandBarrier();
        g_allocation_gate_enabled.store(false, std::memory_order_relaxed);
        return expect(
            producer->followups_applied == 2u &&
                g_gated_new_calls.load(std::memory_order_relaxed) ==
                    second_global_calls_before,
            "observer commands apply only in the next allocation-free barrier");
    }

    [[nodiscard]] bool testZeroByteBarrierObserverAdmission()
    {
        lux::ecs::World world;
        auto& registry = world.registry();
        const auto doomed = registry.create();
        const auto followup_target = registry.create();
        registry.emplace<ZeroByteBarrierTriggerComponent>(doomed);

        lux::ecs::Schedule schedule{world};
        auto added = schedule.addSystem(
            std::make_unique<ZeroByteBarrierObserverSystem>(
                doomed, followup_target));
        if (!expect(
                added.has_value(),
                "zero-byte observer fixture installs producer"))
        {
            return false;
        }
        auto* producer = schedule.get(*added);
        if (!expect(
                producer && producer->queued &&
                    schedule.prepareCommandBarrier(),
                "zero-byte destroy command preflights"))
        {
            return false;
        }

        const auto first_global_calls_before =
            g_gated_new_calls.load(std::memory_order_relaxed);
        g_allocation_gate_enabled.store(true, std::memory_order_relaxed);
        schedule.applyCommandBarrier();
        g_allocation_gate_enabled.store(false, std::memory_order_relaxed);
        const auto after_destroy = registry.memorySnapshot();
        if (!expect(
                !registry.valid(doomed) &&
                    producer->observer_enqueue_count == 1u &&
                    producer->followups_applied == 0u &&
                    after_destroy.armed_reservations == 0u &&
                    g_gated_new_calls.load(std::memory_order_relaxed) ==
                        first_global_calls_before,
                "zero-byte destroy defers observer reservation and apply"))
        {
            return false;
        }

        if (!expect(
                schedule.prepareCommandBarrier(),
                "deferred nonzero observer command preflights"))
        {
            return false;
        }
        const auto armed = registry.memorySnapshot();
        if (!expect(
                armed.armed_reservations == 1u,
                "next barrier owns the deferred observer reservation"))
        {
            return false;
        }

        const auto second_global_calls_before =
            g_gated_new_calls.load(std::memory_order_relaxed);
        g_allocation_gate_enabled.store(true, std::memory_order_relaxed);
        schedule.applyCommandBarrier();
        g_allocation_gate_enabled.store(false, std::memory_order_relaxed);
        const auto published = registry.memorySnapshot();
        return expect(
            producer->followups_applied == 1u &&
                registry.all_of<ZeroByteBarrierFollowupComponent>(
                    followup_target) &&
                registry.get<ZeroByteBarrierFollowupComponent>(
                    followup_target).value == 91u &&
                published.armed_reservations == 0u &&
                published.active_scopes == 0u &&
                published.publication_invariant_failures == 0u &&
                g_gated_new_calls.load(std::memory_order_relaxed) ==
                    second_global_calls_before,
            "deferred observer publishes allocation-free in the next barrier");
    }
}

int main()
{
    if (!testPendingCycleAndStructuredFailure()) return 1;
    if (!testSelfCycle()) return 1;
    if (!testLivePendingCycleIsAtomic()) return 1;
    if (!testUnknownEdges()) return 1;
    if (!testStableSequenceAndBatchGolden()) return 1;
    if (!testDirectMutationPreflight()) return 1;
    if (!testRemovalPrerequisiteGuard()) return 1;
    if (!testStructuredMissingPrerequisite()) return 1;
    if (!testDescriptorCaptureBoundary()) return 1;
    if (!testServicePublishConflictIsAtomic()) return 1;
    if (!testReentrantOperationsAreRejected()) return 1;
    if (!testDynamicBatchMutation()) return 1;
    if (!testSystemFrameTrace()) return 1;
    if (!testCommandBarrierPublicationReservation()) return 1;
    if (!testZeroByteBarrierObserverAdmission()) return 1;

    std::cout << "schedule_topology_test: PASS\n";
    return 0;
}
