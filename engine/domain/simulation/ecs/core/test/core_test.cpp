#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>

namespace
{
    struct Position final
    {
        int value{};
    };

    struct Velocity final
    {
        int value{};
    };

    struct ThrowingComponent final
    {
        ThrowingComponent()
        {
            throw std::runtime_error("component construction failed");
        }
    };

    void testDeterministicDeferredEntities()
    {
        using namespace lux::simulation::ecs;
        Registry registry;
        EcsCommandBuffer commands;
        constexpr std::array capacities{EcsCommandProducerCapacity{4U, 256U}, EcsCommandProducerCapacity{4U, 256U}};
        assert(commands.prepare(capacities));

        DeferredEntity first;
        DeferredEntity second;
        {
            auto writer = commands.begin(1U);
            assert(writer);
            second = writer->create();
            assert(writer->emplace<Position>(second, Position{2}));
        }
        {
            auto writer = commands.begin(0U);
            assert(writer);
            first = writer->create();
            assert(writer->emplace<Position>(first, Position{1}));
        }

        assert(applyEcsCommands(registry, commands));
        const auto first_entity = commands.resolve(first);
        const auto second_entity = commands.resolve(second);
        assert(first_entity && second_entity);
        assert(registry.get<Position>(*first_entity).value == 1);
        assert(registry.get<Position>(*second_entity).value == 2);
        assert(entt::to_integral(*first_entity) < entt::to_integral(*second_entity));
    }

    void testRecordingFailureIsAtomic()
    {
        using namespace lux::simulation::ecs;
        Registry registry;
        EcsCommandBuffer commands;
        constexpr std::array capacities{EcsCommandProducerCapacity{1U, 64U}};
        assert(commands.prepare(capacities));
        {
            auto writer = commands.begin(0U);
            assert(writer);
            const DeferredEntity entity = writer->create();
            assert(entity.valid());
            assert(!writer->emplace<Position>(entity, Position{3}));
        }
        const auto applied = applyEcsCommands(registry, commands);
        assert(!applied);
        assert(applied.error().code == EEcsCommandError::CAPACITY_EXCEEDED);
        assert(registry.storage<Entity>().free_list() == 0U);
    }

    void testActiveWriterAndTokenLifetime()
    {
        using namespace lux::simulation::ecs;
        Registry registry;
        EcsCommandBuffer commands;
        constexpr std::array capacities{EcsCommandProducerCapacity{2U, 64U}};
        assert(commands.prepare(capacities));
        DeferredEntity entity;
        {
            auto writer = commands.begin(0U);
            assert(writer);
            entity = writer->create();
            assert(!applyEcsCommands(registry, commands));
            const auto second = commands.begin(0U);
            assert(!second);
            assert(second.error().code == EEcsCommandError::ACTIVE_WRITER);
        }
        assert(applyEcsCommands(registry, commands));
        assert(commands.resolve(entity));
        commands.discardPending();
        assert(!commands.resolve(entity));
    }

    void testApplyFailuresAreClassified()
    {
        using namespace lux::simulation::ecs;
        Registry registry;
        EcsCommandBuffer commands;
        constexpr std::array capacities{EcsCommandProducerCapacity{2U, 128U}};
        assert(commands.prepare(capacities));
        {
            auto writer = commands.begin(0U);
            assert(writer);
            assert(writer->emplace<Velocity>(static_cast<Entity>(9000U), Velocity{4}));
        }
        auto applied = applyEcsCommands(registry, commands);
        assert(!applied);
        assert(applied.error().code == EEcsCommandError::INVALID_ENTITY);

        assert(commands.prepare(capacities));
        const Entity entity = registry.create();
        {
            auto writer = commands.begin(0U);
            assert(writer);
            assert(writer->emplace<ThrowingComponent>(entity));
        }
        applied = applyEcsCommands(registry, commands);
        assert(!applied);
        assert(applied.error().code == EEcsCommandError::COMPONENT_CONSTRUCTION_FAILURE);
    }

    void testCommitSnapshotAndDeferredGeneration()
    {
        using namespace lux::simulation::ecs;
        Registry registry;
        EcsCommandBuffer commands;
        constexpr std::array capacities{EcsCommandProducerCapacity{8U, 256U}};
        assert(commands.prepare(capacities));
        const auto allocations = commands.allocationEvents();
        struct Observer final
        {
            EcsCommandBuffer& commands;
            DeferredEntity deferred;
            unsigned calls{};
            void constructed(Registry& registry, Entity) noexcept
            {
                ++calls;
                auto writer = commands.begin(0U);
                assert(writer);
                deferred = writer->create();
                assert(writer->emplace<Velocity>(deferred, Velocity{99}));
                // Nested commit cannot mutate the snapshot currently on the native stack.
                assert(!applyEcsCommands(registry, commands));
            }
        } observer{commands};
        auto connection = registry.on_construct<Position>().connect<&Observer::constructed>(observer);
        DeferredEntity first;
        {
            auto writer = commands.begin(0U);
            assert(writer);
            first = writer->create();
            assert(writer->emplace<Position>(first, Position{1}));
        }
        assert(applyEcsCommands(registry, commands));
        assert(observer.calls == 1U);
        assert(commands.resolve(first));
        assert(!commands.resolve(observer.deferred));
        assert(registry.view<Velocity>().size() == 0U);
        assert(first.generation != observer.deferred.generation);
        assert(applyEcsCommands(registry, commands));
        assert(!commands.resolve(first));
        const auto next = commands.resolve(observer.deferred);
        assert(next && registry.get<Velocity>(*next).value == 99);
        assert(applyEcsCommands(registry, commands));
        assert(commands.resolve(observer.deferred) == next);
        assert(commands.allocationEvents() == allocations);
    }
}

int
main()
{
    using lux::simulation::ecs::Entity;
    static_assert(sizeof(Entity) == sizeof(std::uint64_t));
    static_assert(entt::entt_traits<Entity>::entity_mask >= 2'000'000U);

    testDeterministicDeferredEntities();
    testRecordingFailureIsAtomic();
    testActiveWriterAndTokenLifetime();
    testApplyFailuresAreClassified();
    testCommitSnapshotAndDeferredGeneration();
}
