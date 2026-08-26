#include <lux/engine/simulation/ecs/EcsTaskAccess.hpp>
#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <atomic>
#include <cassert>
#include <memory>

namespace
{
    struct Position final
    {
        int value{};
    };

    struct ExternalState final
    {
    };

    struct ProbeSystem final : lux::simulation::StaticSystemAccess<>
    {
        explicit ProbeSystem(std::shared_ptr<std::atomic_int> destroyed)
            : destroyed_(std::move(destroyed))
        {
        }

        ~ProbeSystem()
        {
            destroyed_->fetch_add(1);
        }

        void tick(std::atomic_int& value) noexcept
        {
            value.fetch_add(1, std::memory_order_relaxed);
        }

        std::shared_ptr<std::atomic_int> destroyed_;
    };

    struct ComponentWriterSystem final :
        lux::simulation::StaticSystemAccess<
            lux::simulation::ecs::Write<Position>>
    {
    };

    struct ExternalWriterSystem final :
        lux::simulation::StaticSystemAccess<
            lux::simulation::ExternalWrite<ExternalState>>
    {
    };

    struct CodeLifetimeProbe final
    {
        CodeLifetimeProbe(
            std::shared_ptr<std::atomic_int> destroyed,
            std::shared_ptr<std::atomic_int> released
        )
            : system_destroyed(std::move(destroyed)),
              code_released(std::move(released))
        {
        }

        std::shared_ptr<std::atomic_int> system_destroyed;
        std::shared_ptr<std::atomic_int> code_released;

        ~CodeLifetimeProbe()
        {
            assert(system_destroyed->load() == 1);
            code_released->fetch_add(1);
        }
    };
}

int main()
{
    using namespace lux;

    auto destroyed = std::make_shared<std::atomic_int>(0);
    simulation::SystemRegistry systems;
    auto id = systems.emplace<ProbeSystem>(destroyed);
    assert(id);

    simulation::SystemRegistry foreign_systems;
    assert(!foreign_systems.contains(*id));
    assert(!foreign_systems.erase(*id));
    auto wrong_type = systems.retain<ComponentWriterSystem>(*id);
    assert(!wrong_type);

    auto lease_result = systems.retain<ProbeSystem>(*id);
    assert(lease_result);
    simulation::SystemLease<ProbeSystem> lease = std::move(*lease_result);

    std::atomic_int ticks{};
    task::TaskGraphBuilder builder;
    auto task_id = builder.add(
        simulation::ecs::systemTaskResources<ProbeSystem>(),
        [system = lease, &ticks]() noexcept
        {
            system->tick(ticks);
        }
    );
    assert(task_id);
    auto graph_result = std::move(builder).build();
    assert(graph_result);
    task::TaskGraph graph = std::move(*graph_result);

    // erase() removes membership, but the compiled graph's captured lease keeps
    // the concrete System and code alive.
    lease = {};
    assert(systems.erase(*id));
    assert(!systems.contains(*id));
    assert(!systems.retain<ProbeSystem>(*id));
    assert(destroyed->load() == 0);

    task::TaskExecutor executor(task::TaskExecutorConfig{1U, 0U});
    assert(executor.execute(graph));
    assert(ticks.load() == 1);

    graph = task::TaskGraph{};
    assert(destroyed->load() == 1);

    // The graph lease destroys the System object before releasing the module
    // code-lifetime pin captured by the Registry control block.
    {
        auto system_destroyed = std::make_shared<std::atomic_int>(0);
        auto code_released = std::make_shared<std::atomic_int>(0);
        auto code_lifetime = std::make_shared<CodeLifetimeProbe>(
            system_destroyed,
            code_released
        );
        auto retained_id = systems.emplaceWithLifetime<ProbeSystem>(
            code_lifetime,
            system_destroyed
        );
        assert(retained_id);
        auto retained = systems.retain<ProbeSystem>(*retained_id);
        assert(retained);
        task::TaskGraphBuilder retained_builder;
        auto retained_task = retained_builder.add(
            [system = *retained]() noexcept
            {
                std::atomic_int value{};
                system->tick(value);
            }
        );
        assert(retained_task);
        auto retained_graph_result = std::move(retained_builder).build();
        assert(retained_graph_result);
        task::TaskGraph retained_graph = std::move(*retained_graph_result);
        retained = {};
        code_lifetime.reset();
        assert(systems.erase(*retained_id));
        assert(system_destroyed->load() == 0);
        assert(code_released->load() == 0);
        assert(executor.execute(retained_graph));
        retained_graph = {};
        assert(system_destroyed->load() == 1);
        assert(code_released->load() == 1);
    }

    // System metadata and typed ECS task access must share the same canonical
    // resource identity, otherwise the generic TaskGraph misses the hazard.
    {
        task::TaskGraphBuilder builder;
        auto writer = builder.add(
            simulation::ecs::systemTaskResources<ComponentWriterSystem>(),
            []() noexcept {}
        );
        auto reader = builder.add(
            task::resources(
                simulation::ecs::access<simulation::ecs::Read<Position>>
            ),
            []() noexcept {}
        );
        assert(writer && reader);
        auto result = std::move(builder).build();
        assert(result);
        assert(result->dependencyCount() == 1U);
    }

    // Multiple Journal readers remain independent while a later writer is
    // ordered after both by the canonical changes resource.
    {
        task::TaskGraphBuilder builder;
        auto first_reader = builder.add(
            simulation::ecs::ecsChangesRead(),
            []() noexcept {}
        );
        auto second_reader = builder.add(
            simulation::ecs::ecsChangesRead(),
            []() noexcept {}
        );
        auto writer = builder.add(
            simulation::ecs::ecsChangesWrite(),
            []() noexcept {}
        );
        assert(first_reader && second_reader && writer);
        auto result = std::move(builder).build();
        assert(result);
        assert(result->rootTaskCount() == 2U);
        assert(result->dependencyCount() == 2U);
    }

    {
        task::TaskGraphBuilder builder;
        auto writer = builder.add(
            simulation::ecs::systemTaskResources<ExternalWriterSystem>(),
            []() noexcept {}
        );
        auto reader = builder.add(
            task::resources(
                simulation::ecs::access<
                    simulation::ExternalRead<ExternalState>>
            ),
            []() noexcept {}
        );
        assert(writer && reader);
        auto result = std::move(builder).build();
        assert(result);
        assert(result->dependencyCount() == 1U);
    }
    return 0;
}
