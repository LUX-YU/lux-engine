#include <lux/engine/simulation/SimulationExecution.hpp>

namespace lux::simulation
{
    lux::cxx::expected<void, SimulationStepFailure>
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
        {
            commands.discardPending();
            SimulationStepFailure failure;
            failure.code = ESimulationStepError::TASK_EXECUTION_FAILED;
            failure.task_failure = execution.error();
            return lux::cxx::unexpected(failure);
        }

        if (commands.failed())
        {
            commands.discardPending();
            SimulationStepFailure failure;
            failure.code = ESimulationStepError::COMMAND_RECORDING_FAILED;
            return lux::cxx::unexpected(failure);
        }

        auto applied = ecs::applyEcsCommands(state, journal, commands);
        if (!applied)
        {
            commands.discardPending();
            SimulationStepFailure failure;
            failure.code = ESimulationStepError::COMMAND_APPLY_FAILED;
            failure.command_apply_failure = applied.error();
            return lux::cxx::unexpected(failure);
        }
        return {};
    }
} // namespace lux::simulation
