#include <lux/engine/ecs/EcsTaskAccess.hpp>
#include <lux/engine/ecs/WorldTaskResources.hpp>
#include <lux/engine/task/TaskGraph.hpp>

#include <cassert>
#include <cstdint>

namespace
{
    struct Position final { int value{}; };
    struct Velocity final { int value{1}; };
    inline constexpr auto MovementAccess = lux::ecs::access<
        lux::ecs::Write<Position>,
        lux::ecs::Read<Velocity>>;
}

int main()
{
    lux::ecs::World world{lux::ecs::WorldConfig{{4096U, 64U * 1024U}}};
    auto mutation = world.mutate();
    assert(mutation);
    const auto entity = mutation->create();
    mutation->emplace<Position>(entity);
    mutation->emplace<Velocity>(entity);
    mutation = {};

    lux::ecs::WorldChangeBatch changes;
    assert(changes.prepare(MovementAccess.writeStorages(), 1U));
    lux::ecs::WorldCommandBatch commands;
    assert(commands.prepare(1U));

    lux::task::TaskGraphBuilder builder;
    const auto movement = builder.add(
        lux::task::resources(MovementAccess),
        [&]() noexcept
        {
            for (auto [current, position, velocity] :
                 lux::ecs::taskQuery(world, changes, MovementAccess))
            {
                assert(current == entity);
                position.value += velocity.value;
            }
        }
    );
    const auto publish = builder.add(
        lux::ecs::worldChangesWrite(),
        [&]() noexcept { (void)changes.publish(world); }
    );
    const auto apply = builder.add(
        lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD),
        lux::ecs::worldCommandsWrite(),
        [&]() noexcept { lux::ecs::applyWorldCommands(world, commands); }
    );
    assert(movement && publish && apply);
    assert(builder.before(*movement, *publish));
    assert(builder.before(*publish, *apply));
    auto graph = lux::task::compile(std::move(builder));
    assert(graph);
    lux::task::TaskRunState state;
    assert(lux::task::prepare(state, *graph));
    auto lease = world.beginTaskExecution();
    assert(lease);
    lux::task::InlineTaskExecutor executor;
    assert(lux::task::run(*graph, executor, state));
    lease = {};
    assert(world.get<Position>(entity).value == 1);
    const auto stats = changes.stats();
    assert(stats.lane_binds == 1U);
    assert(stats.journal_stream_binds == 1U);
    assert(stats.record_appends == 1U);
    assert(stats.per_record_lookups == 0U);
}
