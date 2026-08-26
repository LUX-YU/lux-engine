#include <lux/engine/simulation/SimulationExecution.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <cassert>
#include <cstdint>

namespace
{
    struct Position final
    {
        std::int32_t value{};
    };

    struct SetPosition final
    {
        lux::simulation::ecs::Entity entity{};
        std::int32_t value{};

        void apply(
            lux::simulation::ecs::SimulationEcsMutation& mutation
        ) noexcept
        {
            mutation.emplace<Position>(entity, value);
        }
    };
}

int main()
{
    using namespace lux;

    simulation::ecs::EcsState state;
    simulation::ecs::EcsChangeJournal journal({4096U, 65536U});
    simulation::ecs::EcsCommandBatch commands;
    assert(commands.prepare(1U));
    simulation::ecs::ChangeCursor<Position> cursor;
    assert(
        journal.read(cursor).status() ==
        simulation::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );

    simulation::ecs::Entity entity{};
    {
        auto mutation = state.mutate();
        assert(mutation);
        entity = mutation->create();
    }

    task::TaskGraphBuilder builder;
    auto producer = builder.add(
        [&]() noexcept
        {
            assert(state.find<Position>(entity) == nullptr);
            auto scope = commands.begin(0U);
            assert(scope);
            assert(
                scope->commands().push(SetPosition{entity, 42}) ==
                simulation::ecs::ECommandResult::ACCEPTED
            );
        }
    );
    assert(producer);
    auto graph = std::move(builder).build();
    assert(graph);

    task::TaskExecutor executor(task::TaskExecutorConfig{1U, 0U});
    auto step = simulation::executeSimulationStep(
        executor,
        *graph,
        state,
        journal,
        commands
    );
    assert(step);
    assert(state.get<Position>(entity).value == 42);

    const auto changes = journal.read(cursor);
    assert(changes.status() == simulation::ecs::EChangeReadStatus::CURRENT);
    assert(changes.size() == 1U);

    return 0;
}
