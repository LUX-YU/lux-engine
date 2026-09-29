#pragma once

#include <lux/engine/scene/Clock.hpp>
#include <lux/engine/scene/SceneCapabilityProvider.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/scene/SceneInstanceLease.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/visibility.h>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/process/Timer.hpp>

#include <functional>

namespace lux::process
{
    class ExecutionRuntime;
}

namespace lux::scene
{
    enum class ESceneBuildError : std::uint8_t
    {
        INVALID_DESCRIPTION,
        INVALID_WORLD,
        INVALID_SIMULATION,
        INVALID_PROVIDER,
        SIMULATION_BUILD_FAILURE,
        SCENE_SYSTEM_BUILD_FAILURE,
        ALLOCATION_FAILURE,
        IDENTITY_EXHAUSTED,
        UNSUPPORTED_WORLD_TYPE,
    };

    struct SceneBuildFailure final
    {
        ESceneBuildError code{ESceneBuildError::INVALID_DESCRIPTION};
        simulation::SimulationSystemBuildFailure simulation{};
        SceneSystemBuildFailure scene_system{};
        std::uint64_t subject_hash{};
    };

    enum class ESceneDrivePhase : std::uint8_t
    {
        NONE,
        MAINTENANCE,
        SIMULATION,
        SYNCHRONIZATION,
        STABLE,
        PUBLICATION,
    };

    enum class ESceneDriveError : std::uint8_t
    {
        REENTRANT,
        STOPPED,
        INVALID_TIME,
    };

    struct SceneDriveFailure final
    {
        ESceneDrivePhase phase{ESceneDrivePhase::NONE};
        std::variant<simulation::SimulationExecutionFailure, SceneExecutionFailure, ESceneDriveError> cause;
    };

    struct SceneDriveSnapshot final
    {
        ESceneDrivePhase phase{ESceneDrivePhase::NONE};
        simulation::SimulationTime time;
        std::uint64_t simulation_completed{}, stable_completed{}, publication_completed{};
        std::chrono::nanoseconds active_work{}, publication_wait{}, longest_call{};
        lux::cxx::expected<void, SceneDriveFailure> result;
    };

    enum class ESceneRuntimeError : std::uint8_t
    {
        INVALID_ID,
        WRONG_DOMAIN,
        WRONG_THREAD,
        BUSY,
        STOPPED,
        INVALID_INPUT,
        IDENTITY_EXHAUSTED,
        CAPACITY
    };

    struct SceneRuntimeFailure final
    {
        using VCause = std::variant<
            ESceneRuntimeError,
            SceneBuildFailure,
            SceneDriveFailure,
            EClockError,
            task::TaskExecutorFailure,
            process::ETimerError>;
        SceneInstanceId scene;
        VCause cause;
    };

    template <class T> using SceneRuntimeResult = lux::cxx::expected<T, SceneRuntimeFailure>;

    struct SceneStepTicket final
    {
        SceneInstanceId scene;
        std::uint64_t serial{}, simulation_completed{};
        friend auto operator<=>(const SceneStepTicket&, const SceneStepTicket&) = default;
    };

    enum class ESceneStepState : std::uint8_t
    {
        QUEUED,
        EXECUTING,
        COMPLETED,
        FAILED,
        CANCELLED
    };

    struct SceneStepStatus final
    {
        ESceneStepState state{ESceneStepState::QUEUED};
        SceneRuntimeResult<void> result;
    };

    // Owner-thread scene composition. Registry/clock borrows end before driveFrame,
    // structure changes or owner waits. A lease's runtime must outlive the lease.
    class LUX_ENGINE_SCENE_PUBLIC SceneRuntime final
    {
    public:
        class LUX_ENGINE_SCENE_PUBLIC Builder final
        {
        public:
            Builder& setDescription(std::shared_ptr<const SceneDescription>) noexcept;
            Builder& setWorld(std::shared_ptr<const world::WorldDescription>) noexcept;
            Builder& setSimulation(std::shared_ptr<const simulation::SimulationDescription>) noexcept;
            Builder&
            setRegistrations(const simulation::ecs::ComponentSchemaSet&, const simulation::SimulationSystemRegistry&, std::span<const SceneSystemRegistration>) noexcept;
            Builder& setProviders(std::span<const SceneCapabilityProvider>) noexcept;

            template <Clock T>
                requires std::constructible_from<VSimulationClock, T>
            Builder& setClock(T value) noexcept
            {
                clock_ = std::move(value);
                return *this;
            }

            // Registration/provider borrows must stay alive until build returns.
            [[nodiscard]] SceneRuntimeResult<SceneInstanceLease> build() noexcept;

        private:
            friend class SceneRuntime;
            explicit Builder(SceneRuntime& runtime) noexcept : runtime_(runtime) {}
            SceneRuntime& runtime_;
            std::shared_ptr<const SceneDescription> description_;
            std::shared_ptr<const world::WorldDescription> world_;
            std::shared_ptr<const simulation::SimulationDescription> simulation_;
            const simulation::ecs::ComponentSchemaSet* components_{};
            const simulation::SimulationSystemRegistry* simulation_systems_{};
            std::span<const SceneSystemRegistration> scene_systems_;
            std::span<const SceneCapabilityProvider> providers_;
            VSimulationClock clock_;
        };

        using CreateResult = SceneRuntimeResult<std::unique_ptr<SceneRuntime>>;
        using DriveResult = SceneRuntimeResult<std::span<const SceneRuntimeFailure>>;
        [[nodiscard]] static CreateResult create(process::ExecutionRuntime&, task::TaskExecutorConfig) noexcept;
        ~SceneRuntime() noexcept;
        SceneRuntime(const SceneRuntime&) = delete;
        SceneRuntime& operator=(const SceneRuntime&) = delete;

        [[nodiscard]] Builder builder() noexcept
        {
            return Builder{*this};
        }
        [[nodiscard]] SceneRuntimeResult<std::reference_wrapper<simulation::ecs::Registry>> borrowInstance(
            SceneInstanceId
        ) noexcept;
        [[nodiscard]] SceneRuntimeResult<std::reference_wrapper<const simulation::ecs::Registry>> borrowInstance(
            SceneInstanceId
        ) const noexcept;
        [[nodiscard]] SceneRuntimeResult<std::reference_wrapper<const VSimulationClock>> borrowClock(SceneInstanceId
        ) const noexcept;
        [[nodiscard]] SceneRuntimeResult<void> pauseSimulation(SceneInstanceId) noexcept;
        [[nodiscard]] SceneRuntimeResult<void> resumeSimulation(SceneInstanceId) noexcept;
        // At most 32 unacknowledged tickets per instance. Only paused instances admit steps.
        [[nodiscard]] SceneRuntimeResult<SceneStepTicket> requestStep(SceneInstanceId) noexcept;
        [[nodiscard]] SceneRuntimeResult<SceneStepStatus> stepStatus(SceneStepTicket) const noexcept;
        [[nodiscard]] SceneRuntimeResult<void> acknowledgeStep(SceneStepTicket) noexcept;
        // May be called inside a system callback; no callback or erasure happens here.
        [[nodiscard]] SceneRuntimeResult<InstanceRetirement> retireInstance(SceneInstanceId) noexcept;
        [[nodiscard]] DriveResult driveFrame() noexcept;

    private:
        struct Impl;
        explicit SceneRuntime(std::unique_ptr<Impl>) noexcept;
        [[nodiscard]] SceneRuntimeResult<SceneInstanceLease> build(const Builder&) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
