#pragma once

#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/visibility.h>
#include <lux/engine/simulation/Simulation.hpp>

#include <chrono>
#include <variant>

namespace lux::scene
{
class SceneInstance;

enum class ESceneDriveState : std::uint8_t
{
    PAUSED,
    RUNNING,
    STOPPING,
    STOPPED,
    FAILED,
};

enum class ESceneDrivePhase : std::uint8_t
{
    NONE,
    MAINTENANCE,
    SIMULATION,
    DERIVATION,
    STABLE,
    PUBLICATION,
};

enum class ESceneControlError : std::uint8_t
{
    BUSY,
    STOPPED,
    WRONG_MODE,
    INVALID_STATE,
};

struct SceneDriveFailure final
{
    ESceneDrivePhase phase{ESceneDrivePhase::NONE};
    std::variant<simulation::SimulationExecutionFailure, SceneExecutionFailure> cause;
};

struct SceneDriveSnapshot final
{
    ESceneDriveState state{ESceneDriveState::PAUSED};
    ESceneDrivePhase phase{ESceneDrivePhase::NONE};
    bool pause_pending{};
    bool step_pending{};
    simulation::SimulationClockSnapshot clock;
    std::uint64_t simulation_completed{}, stable_completed{}, publication_completed{};
    std::uint64_t refresh_completed{}; // Paused updates are not deduplicated by step index.
    std::chrono::nanoseconds active_work{}, publication_wait{}, longest_call{};
    lux::cxx::expected<void, SceneDriveFailure> result;
};

struct SceneAdvanceBudget final
{
    std::size_t system_calls{64};
    std::size_t new_steps{1};
    std::size_t publications{8};
    std::size_t resource_steps{16};
};

// One synchronous Main-thread progression owner. Requests only record intent.
// This object borrows its executor and never owns instances or per-instance state.
class LUX_ENGINE_SCENE_PUBLIC SceneDriver final
{
  public:
    explicit SceneDriver(task::TaskExecutor &executor) noexcept : executor_(executor)
    {
    }
    using ControlResult = lux::cxx::expected<void, ESceneControlError>;
    [[nodiscard]] ControlResult play(SceneInstance &) noexcept;
    [[nodiscard]] ControlResult pause(SceneInstance &) noexcept;
    [[nodiscard]] ControlResult step(SceneInstance &) noexcept;
    void stop(SceneInstance &) noexcept;
    void invalidate(SceneInstance &) noexcept;
    [[nodiscard]] ESceneProgress advance(SceneInstance &, std::chrono::steady_clock::time_point now,
                                         SceneAdvanceBudget &budget) noexcept;

  private:
    task::TaskExecutor &executor_;
};
} // namespace lux::scene
