#include <lux/engine/simulation/SimulationExecution.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <cassert>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

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

    struct UpdatePosition final
    {
        lux::simulation::ecs::Entity entity{};
        std::int32_t value{};

        void apply(
            lux::simulation::ecs::SimulationEcsMutation& mutation
        ) noexcept
        {
            mutation.update<Position>(
                entity,
                [this](Position& position) noexcept
                {
                    position.value = value;
                }
            );
        }
    };
}

int main()
{
    using namespace lux;

    simulation::ecs::EcsState state;
    simulation::ecs::EcsChangeJournal journal({4096U, 65536U});
    simulation::ecs::EcsCommandBatch commands;
    constexpr std::array command_capacities{
        simulation::ecs::EcsCommandProducerCapacity{1U, 64U}
    };
    assert(commands.prepare(command_capacities));
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

    task::TaskGraphBuilder empty_builder;
    auto empty_graph = std::move(empty_builder).build();
    assert(empty_graph);

    // A poisoned recording batch is discarded instead of partially applied.
    assert(commands.prepare(command_capacities));
    task::TaskGraphBuilder overflow_builder;
    auto overflow_task = overflow_builder.add(
        [&]() noexcept
        {
            auto scope = commands.begin(0U);
            assert(scope);
            assert(
                scope->commands().push(UpdatePosition{entity, 50}) ==
                simulation::ecs::ECommandResult::ACCEPTED
            );
            assert(
                scope->commands().push(UpdatePosition{entity, 51}) ==
                simulation::ecs::ECommandResult::CAPACITY_EXCEEDED
            );
        }
    );
    assert(overflow_task);
    auto overflow_graph = std::move(overflow_builder).build();
    assert(overflow_graph);
    auto overflow_step = simulation::executeSimulationStep(
        executor,
        *overflow_graph,
        state,
        journal,
        commands
    );
    assert(!overflow_step);
    assert(
        overflow_step.error().code ==
        simulation::ESimulationStepError::COMMAND_RECORDING_FAILED
    );
    assert(state.get<Position>(entity).value == 42);
    assert(simulation::executeSimulationStep(
        executor,
        *empty_graph,
        state,
        journal,
        commands
    ));
    assert(state.get<Position>(entity).value == 42);

    // A busy ECS state is a typed apply failure and also discards pending work.
    assert(commands.prepare(command_capacities));
    {
        auto scope = commands.begin(0U);
        assert(scope);
        assert(
            scope->commands().push(UpdatePosition{entity, 60}) ==
            simulation::ecs::ECommandResult::ACCEPTED
        );
    }
    auto held_mutation = state.mutate();
    assert(held_mutation);
    auto busy_step = simulation::executeSimulationStep(
        executor,
        *empty_graph,
        state,
        journal,
        commands
    );
    assert(!busy_step);
    assert(
        busy_step.error().code ==
        simulation::ESimulationStepError::COMMAND_APPLY_FAILED
    );
    assert(
        busy_step.error().command_apply_failure.code ==
        simulation::ecs::EEcsCommandApplyError::STATE_NOT_IDLE
    );
    *held_mutation = {};
    assert(simulation::executeSimulationStep(
        executor,
        *empty_graph,
        state,
        journal,
        commands
    ));
    assert(state.get<Position>(entity).value == 42);

    // Applying from a non-owner thread returns WRONG_THREAD, never aborts.
    assert(commands.prepare(command_capacities));
    {
        auto scope = commands.begin(0U);
        assert(scope);
        assert(
            scope->commands().push(UpdatePosition{entity, 70}) ==
            simulation::ecs::ECommandResult::ACCEPTED
        );
    }
    simulation::ESimulationStepError wrong_thread_code{};
    simulation::ecs::EEcsCommandApplyError wrong_thread_apply{};
    std::thread wrong_thread([&]() noexcept
    {
        auto result = simulation::executeSimulationStep(
            executor,
            *empty_graph,
            state,
            journal,
            commands
        );
        assert(!result);
        wrong_thread_code = result.error().code;
        wrong_thread_apply = result.error().command_apply_failure.code;
    });
    wrong_thread.join();
    assert(wrong_thread_code == simulation::ESimulationStepError::COMMAND_APPLY_FAILED);
    assert(wrong_thread_apply == simulation::ecs::EEcsCommandApplyError::WRONG_THREAD);
    assert(simulation::executeSimulationStep(
        executor,
        *empty_graph,
        state,
        journal,
        commands
    ));
    assert(state.get<Position>(entity).value == 42);

    // An executor start failure discards already-recorded commands.
    assert(commands.prepare(command_capacities));
    {
        auto scope = commands.begin(0U);
        assert(scope);
        assert(
            scope->commands().push(UpdatePosition{entity, 80}) ==
            simulation::ecs::ECommandResult::ACCEPTED
        );
    }
    std::atomic<bool> entered{};
    std::atomic<bool> release{};
    task::TaskGraphBuilder blocking_builder;
    auto blocking_task = blocking_builder.add([&]() noexcept
    {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    assert(blocking_task);
    auto blocking_graph = std::move(blocking_builder).build();
    assert(blocking_graph);
    task::TaskExecutor busy_executor(task::TaskExecutorConfig{1U, 1U});
    std::thread executing([&]() noexcept
    {
        assert(busy_executor.execute(*blocking_graph));
    });
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    auto executor_failure = simulation::executeSimulationStep(
        busy_executor,
        *empty_graph,
        state,
        journal,
        commands
    );
    assert(!executor_failure);
    assert(
        executor_failure.error().code ==
        simulation::ESimulationStepError::TASK_EXECUTION_FAILED
    );
    release.store(true, std::memory_order_release);
    executing.join();
    assert(simulation::executeSimulationStep(
        busy_executor,
        *empty_graph,
        state,
        journal,
        commands
    ));
    assert(state.get<Position>(entity).value == 42);

    return 0;
}
