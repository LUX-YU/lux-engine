#pragma once

#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/simulation/core/visibility.h>
#include <lux/engine/task/TaskExecutor.hpp>

#include <lux/cxx/compile_time/expected.hpp>

namespace lux::simulation
{
    /**
     * Execute one already-composed synchronous Simulation step.
     *
     * The TaskGraph owns all task dependencies, resource hazards and explicit
     * change-publication tasks. Deferred ECS commands are applied only after the
     * executor has returned successfully and is therefore quiescent. A graph
     * start failure leaves the command batch unapplied; canonical mutations made
     * by tasks that did run are intentionally not rolled back.
     */
    [[nodiscard]] LUX_ENGINE_SIMULATION_CORE_PUBLIC
    lux::cxx::expected<void, task::TaskExecutorFailure>
    executeSimulationStep(
        task::TaskExecutor& executor,
        const task::TaskGraph& graph,
        ecs::EcsState& state,
        ecs::EcsChangeJournal& journal,
        ecs::EcsCommandBatch& commands
    ) noexcept;
} // namespace lux::simulation
