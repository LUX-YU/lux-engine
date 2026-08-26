#include <lux/engine/simulation/SimulationExecution.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <utility>

int main()
{
    lux::task::TaskGraphBuilder builder;
    auto graph = std::move(builder).build();
    if (!graph)
        return 1;

    lux::task::TaskExecutor executor({0U, 0U});
    lux::simulation::ecs::EcsState state;
    lux::simulation::ecs::EcsChangeJournal journal({4096U, 65536U});
    lux::simulation::ecs::EcsCommandBatch commands;
    if (!commands.prepare(
            std::span<const lux::simulation::ecs::EcsCommandProducerCapacity>{}
        ))
        return 2;
    return lux::simulation::executeSimulationStep(
        executor,
        *graph,
        state,
        journal,
        commands
    ) ? 0 : 3;
}
