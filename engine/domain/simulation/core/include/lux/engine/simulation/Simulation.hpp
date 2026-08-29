#pragma once

#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/core/visibility.h>
#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>

namespace lux::simulation
{
    enum class ESimulationExecutionError : std::uint8_t
    {
        TASK_EXECUTOR_FAILURE,
        SYSTEM_TASK_FAILURE,
        ECS_COMMAND_FAILURE,
    };

    struct SimulationExecutionFailure final
    {
        ESimulationExecutionError code{ESimulationExecutionError::TASK_EXECUTOR_FAILURE};
        SystemInstanceId system{};
        task::TaskExecutorFailure task_executor;
        ecs::EcsCommandFailure ecs_command;
    };

    class LUX_ENGINE_SIMULATION_CORE_PUBLIC Simulation final
    {
    public:
        Simulation(Simulation&&) noexcept;
        Simulation& operator=(Simulation&&) noexcept;
        ~Simulation() noexcept;

        Simulation(const Simulation&) = delete;
        Simulation& operator=(const Simulation&) = delete;

        [[nodiscard]] static lux::cxx::expected<Simulation, SystemBuildFailure> create(
            ecs::Registry& registry,
            std::shared_ptr<const SimulationDescription> description,
            const SystemRegistry& system_types
        ) noexcept;

        [[nodiscard]] const SimulationDescription& description() const noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationExecutionFailure>
        execute(task::TaskExecutor& executor) noexcept;

    private:
        struct Impl;
        explicit Simulation(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;

        friend class SimulationBuilder;
    };
} // namespace lux::simulation
