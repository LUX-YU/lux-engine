#pragma once

#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/visibility.h>
#include <lux/engine/simulation/Simulation.hpp>

#include <chrono>
#include <variant>

namespace lux::scene
{
    class SceneInstance;

    enum class ESceneTickResult : std::uint8_t
    {
        EXECUTED,
        DEFERRED,
    };

    // Main-thread execution and maintenance. The host owns playback and time scheduling.
    // This object borrows its executor and never owns instances or per-instance state.
    class LUX_ENGINE_SCENE_PUBLIC SceneDriver final
    {
    public:
        explicit SceneDriver(task::TaskExecutor& executor) noexcept : executor_(executor) {}
        using TickResult = lux::cxx::expected<ESceneTickResult, SceneDriveFailure>;
        // Absolute scene time, not wall time. Equal time still executes one step.
        // DEFERRED neither executes nor queues the request; maintain/publish before retrying.
        // EXECUTED completes Simulation synchronously; publish finishes its stable/publication stages.
        [[nodiscard]] TickResult tick(SceneInstance&, simulation::SimulationDuration time) noexcept;
        // Adopt ready inputs without executing Simulation or publishing.
        [[nodiscard]] ESceneProgress maintain(SceneInstance&) noexcept;
        // Synchronize changed data, then continue stable/publication. Never executes Simulation.
        // COMPLETE describes this traversal, not asynchronous/GPU retirement.
        [[nodiscard]] ESceneProgress publish(SceneInstance&) noexcept;

    private:
        task::TaskExecutor& executor_;
    };
} // namespace lux::scene
