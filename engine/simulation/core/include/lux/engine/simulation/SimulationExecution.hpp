#pragma once

#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/simulation/core/visibility.h>
#include <lux/engine/task/TaskExecutor.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>

namespace lux::simulation
{
    enum class ESimulationStepError : std::uint8_t
    {
        TASK_EXECUTION_FAILED,
        COMMAND_RECORDING_FAILED,
        COMMAND_APPLY_FAILED,
    };

    struct SimulationStepFailure final
    {
        ESimulationStepError code{
            ESimulationStepError::TASK_EXECUTION_FAILED};
        task::TaskExecutorFailure task_failure{};
        ecs::EcsCommandApplyFailure command_apply_failure{};
    };

    /**
     * Execute one already-composed synchronous Simulation step.
     *
     * The TaskGraph owns all task dependencies, resource hazards and explicit
     * change-publication tasks. Deferred ECS commands are applied only after the
     * executor has returned successfully and is therefore quiescent. A graph
     * start failure discards the command batch; canonical mutations made by
     * tasks that did run are intentionally not rolled back.
     */
    [[nodiscard]] LUX_ENGINE_SIMULATION_CORE_PUBLIC
    lux::cxx::expected<void, SimulationStepFailure>
    executeSimulationStep(
        task::TaskExecutor& executor,
        const task::TaskGraph& graph,
        ecs::EcsState& state,
        ecs::EcsChangeJournal& journal,
        ecs::EcsCommandBatch& commands
    ) noexcept;
} // namespace lux::simulation
