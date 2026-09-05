#pragma once

#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/SimulationClock.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/composition/visibility.h>
#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/scripting/ScriptApiCapability.hpp>
#include <lux/engine/simulation/scripting/ScriptEndpointBridge.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace lux::simulation
{
    enum class ESimulationExecutionError : std::uint8_t
    {
        INVALID_STEP_TIME,
        TASK_EXECUTOR_FAILURE,
        SYSTEM_TASK_FAILURE,
        ECS_COMMAND_FAILURE,
        NOT_SEALED,
        STOPPED,
    };

    struct SimulationExecutionFailure final
    {
        ESimulationExecutionError code{ESimulationExecutionError::TASK_EXECUTOR_FAILURE};
        lux::system::SystemInstanceId system{};
        task::TaskExecutorFailure task_executor;
        ecs::EcsCommandFailure ecs_command;
    };

    struct SimulationHookCallbacks final
    {
        void* context{};
        bool (*before)(void*, const SimulationClockSnapshot&, bool stable_resume) noexcept{};
        bool (*after)(void*, const SimulationClockSnapshot&, bool stable_resume) noexcept{};
        bool (*committed)(void*, const SimulationClockSnapshot&) noexcept{};
    };

    class SimulationHookConnection final
    {
    public:
        SimulationHookConnection() noexcept = default;
        SimulationHookConnection(const SimulationHookConnection&) = delete;
        SimulationHookConnection& operator=(const SimulationHookConnection&) = delete;
        SimulationHookConnection(SimulationHookConnection&& other) noexcept
            : context_(std::exchange(other.context_, nullptr)), disconnect_(other.disconnect_)
        {}
        SimulationHookConnection& operator=(SimulationHookConnection&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                context_ = std::exchange(other.context_, nullptr);
                disconnect_ = other.disconnect_;
            }
            return *this;
        }
        ~SimulationHookConnection() noexcept { reset(); }
        void reset() noexcept
        {
            if (context_ != nullptr)
                disconnect_(std::exchange(context_, nullptr));
        }
    private:
        SimulationHookConnection(void* context, void (*disconnect)(void*) noexcept) noexcept
            : context_(context), disconnect_(disconnect)
        {}
        void* context_{};
        void (*disconnect_)(void*) noexcept{};
        friend class Simulation;
    };

    class LUX_ENGINE_SIMULATION_COMPOSITION_PUBLIC Simulation final
    {
    public:
        Simulation(Simulation&&) noexcept;
        Simulation& operator=(Simulation&&) noexcept;
        ~Simulation() noexcept;

        Simulation(const Simulation&) = delete;
        Simulation& operator=(const Simulation&) = delete;

        [[nodiscard]] static lux::cxx::expected<Simulation, SimulationSystemBuildFailure> create(
            ecs::Registry& registry,
            std::shared_ptr<const SimulationDescription> description,
            const SimulationSystemRegistry& system_types
        ) noexcept;

        [[nodiscard]] const SimulationDescription& description() const noexcept;

        [[nodiscard]] std::span<const script::ScriptApiCapabilityPublication>
        scriptApiCapabilities() const noexcept;

        [[nodiscard]] std::span<const script::ScriptHookEndpointDescriptor>
        scriptHookEndpoints() const noexcept;

        [[nodiscard]] std::span<const script::ScriptEventEndpointDescriptor>
        scriptEventEndpoints() const noexcept;

        [[nodiscard]] const SimulationClock& clock() const noexcept;

        // The connection must be released while paused and before this Simulation is destroyed.
        [[nodiscard]] lux::cxx::expected<SimulationHookConnection, SimulationSystemBuildFailure>
        bindHookCallbacks(SimulationHookCallbacks callbacks) noexcept;
        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> seal() noexcept;
        void stop() noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationExecutionFailure>
        execute(task::TaskExecutor& executor, SimulationDuration effective_delta) noexcept;

    private:
        struct Impl;
        explicit Simulation(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;

        friend class SimulationBuilder;
    };
} // namespace lux::simulation
