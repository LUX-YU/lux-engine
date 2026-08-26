#include <lux/engine/simulation/SimulationExecution.hpp>

namespace lux::simulation
{
    lux::cxx::expected<void, task::TaskExecutorFailure>
    executeSimulationStep(
        task::TaskExecutor& executor,
        const task::TaskGraph& graph,
        ecs::EcsState& state,
        ecs::EcsChangeJournal& journal,
        ecs::EcsCommandBatch& commands
    ) noexcept
    {
        auto execution = executor.execute(graph);
        if (!execution)
            return lux::cxx::unexpected(execution.error());

        ecs::applyEcsCommands(state, journal, commands);
        return {};
    }
} // namespace lux::simulation
