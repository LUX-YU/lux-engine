#include <lux/engine/simulation/ecs/EcsTaskAccess.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <cassert>
#include <cstdint>

namespace
{
    struct Position final { int value{}; };
    struct Velocity final { int value{1}; };
    inline constexpr auto MovementAccess = lux::simulation::ecs::access<
        lux::simulation::ecs::Write<Position>,
        lux::simulation::ecs::Read<Velocity>>;
    struct MovementSystem final
    {
        inline static constexpr auto Access =
            lux::simulation::ecs::makeSystemAccessSpec<
                lux::simulation::ecs::Write<Position>,
                lux::simulation::ecs::Read<Velocity>>();
    };
}

int main()
{
    lux::simulation::ecs::EcsState world;
    lux::simulation::ecs::EcsChangeJournal journal(
        lux::simulation::ecs::EcsChangeHistoryBudget{4096U, 64U * 1024U}
    );
    auto mutation = world.mutate();
    assert(mutation);
    const auto entity = mutation->create();
    mutation->emplace<Position>(entity);
    mutation->emplace<Velocity>(entity);
    mutation = {};

    lux::simulation::ecs::EcsChangeBatch changes;
    assert(changes.prepare(MovementAccess.writeStorages(), 1U));
    lux::simulation::ecs::EcsCommandBatch commands;
    assert(commands.prepare(1U));

    lux::task::TaskGraphBuilder builder;
    const auto movement = builder.add(
        lux::simulation::ecs::systemTaskResources<MovementSystem>(),
        [&]() noexcept
        {
            for (auto [current, position, velocity] :
                 lux::simulation::ecs::taskQuery(world, changes, MovementAccess))
            {
                assert(current == entity);
                position.value += velocity.value;
            }
        }
    );
    const auto publish = builder.add(
        lux::task::dependsOn(*movement),
        lux::simulation::ecs::ecsChangesWrite(),
        [&]() noexcept { (void)changes.publish(journal); }
    );
    const auto apply = builder.add(
        lux::task::dependsOn(*publish),
        lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD),
        lux::simulation::ecs::ecsCommandsWrite(),
        [&]() noexcept
        {
            lux::simulation::ecs::applyEcsCommands(world, journal, commands);
        }
    );
    assert(movement && publish && apply);
    auto graph = std::move(builder).build();
    assert(graph);
    lux::task::TaskExecutor executor({0U, graph->taskCount()});
    assert(executor.execute(*graph));
    assert(world.get<Position>(entity).value == 1);
    const auto stats = changes.stats();
    assert(stats.lane_binds == 1U);
    assert(stats.journal_stream_binds == 1U);
    assert(stats.record_appends == 1U);
    assert(stats.per_record_lookups == 0U);
}
