#include <lux/engine/ecs/EcsTaskAccess.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemTaskResources.hpp>
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

    struct ProbeSystem final : lux::ecs::StaticSystemAccess<>
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
        lux::ecs::StaticSystemAccess<lux::ecs::Write<Position>>
    {
    };

    struct ExternalWriterSystem final :
        lux::ecs::StaticSystemAccess<lux::ecs::ExternalWrite<ExternalState>>
    {
    };
}

int main()
{
    using namespace lux;

    auto destroyed = std::make_shared<std::atomic_int>(0);
    ecs::SystemRegistry systems;
    auto id = systems.emplace<ProbeSystem>(destroyed);
    assert(id);

    auto lease_result = systems.retain<ProbeSystem>(*id);
    assert(lease_result);
    ecs::SystemLease<ProbeSystem> lease = std::move(*lease_result);

    std::atomic_int ticks{};
    task::TaskGraphBuilder builder;
    auto task_id = builder.add(
        ecs::systemTaskResources<ProbeSystem>(),
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
    assert(destroyed->load() == 0);

    task::TaskExecutor executor(task::TaskExecutorConfig{.worker_count = 1U});
    assert(executor.execute(graph));
    assert(ticks.load() == 1);

    graph = task::TaskGraph{};
    assert(destroyed->load() == 1);

    // System metadata and typed ECS task access must share the same canonical
    // resource identity, otherwise the generic TaskGraph misses the hazard.
    {
        task::TaskGraphBuilder builder;
        auto writer = builder.add(
            ecs::systemTaskResources<ComponentWriterSystem>(),
            []() noexcept {}
        );
        auto reader = builder.add(
            task::resources(ecs::access<ecs::Read<Position>>),
            []() noexcept {}
        );
        assert(writer && reader);
        auto result = std::move(builder).build();
        assert(result);
        assert(result->dependencyCount() == 1U);
    }

    {
        task::TaskGraphBuilder builder;
        auto writer = builder.add(
            ecs::systemTaskResources<ExternalWriterSystem>(),
            []() noexcept {}
        );
        auto reader = builder.add(
            task::resources(ecs::access<ecs::ExternalRead<ExternalState>>),
            []() noexcept {}
        );
        assert(writer && reader);
        auto result = std::move(builder).build();
        assert(result);
        assert(result->dependencyCount() == 1U);
    }
    return 0;
}
