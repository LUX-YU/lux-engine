#pragma once

#include <exec/task.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/simulation/SimulationTime.hpp>

#include <chrono>
#include <functional>
#include <memory>

namespace lux::scene
{
    class SceneInstance;
}

namespace lux::engine
{
    enum class EEngineStage
    {
        BEGIN_UPDATE,
        SIMULATION_FINISHED,
        END_UPDATE,
        UPDATE_ABORTED
    };

    class EngineContext;

    // Owns externally assembled scenes. EngineContext outlives Engine and all accepted removals.
    class Engine final
    {
    public:
        class SceneBuilder;
        using Duration          = simulation::SimulationDuration;
        using TimePoint         = std::chrono::steady_clock::time_point;
        using Result            = cxx::expected<void, error::Error>;
        using AddSceneResult    = cxx::expected<scene::SceneInstanceId, error::Error>;
        using EngineStageCallback = std::function<Result()>;
        using EngineStageAsyncCallback = std::function<exec::task<Result>()>;
        using EventPump         = std::function<void()>;
        using EventWait         = std::function<void(TimePoint)>;

        explicit Engine(EngineContext& context) noexcept;
        // Final Main-thread owner barrier, outside run/dispatch: retires remaining scenes before destruction.
        // Context and all external dependencies borrowed by those scenes must still be alive.
        ~Engine() noexcept;
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        // Register before run. Captures outlive Engine. Callbacks must not reenter the event loop.
        // Only BEGIN_UPDATE accepts asynchronous admission; registration order is preserved.
        void addEngineStageCallback(EngineStageCallback callback, EEngineStage stage) noexcept;
        void addEngineStageCallback(EngineStageAsyncCallback callback, EEngineStage stage) noexcept;

        // Main-thread safe point only. Builds all graphs before publication. Constructors do not start work.
        // Adoption binds tasks and schedules initialization; query its result with whenSceneInitialized.
        // A Scene does not execute Tick/Collector work until initialization succeeds.
        // Success consumes candidate; a recoverable failure leaves it with the caller.
        [[nodiscard]] AddSceneResult
        addScene(std::unique_ptr<scene::SceneInstance>& candidate, bool simulation_paused = false) noexcept;

        // Main structural safe point only, outside System/graph traversal. Pausing leaves IO/SceneSystems active.
        [[nodiscard]] Result setSimulationPaused(scene::SceneInstanceId scene, bool paused) noexcept;
        
        // Initialized, paused Scene; positive representable delta. Completed work is never rolled back.
        [[nodiscard]] Result advanceSimulation(scene::SceneInstanceId scene, Duration delta) noexcept;
        
        // One in-flight waiter; terminal results remain readable. Engine outlives accepted delivery.
        [[nodiscard]] exec::task<Result> whenSceneInitialized(scene::SceneInstanceId scene) noexcept;

        // Synchronous borrow at an existing Main structural safe point, outside TaskGraph/System traversal.
        // The callback must not retain Scene/Registry pointers or reenter the event pump.
        [[nodiscard]] Result
        visitScene(scene::SceneInstanceId id, const std::function<Result(scene::SceneInstance&)>& visitor) noexcept;

        // Start on Main; removal is deferred to the next execution-dispatch safe point.
        // Keep Engine alive until completion. Accepted removal drains even if its caller requests stop.
        // Do not await a Scene's removal from one of that same Scene's tracked tasks.
        [[nodiscard]] exec::task<Result> removeScene(scene::SceneInstanceId id) noexcept;

        // One run per Engine. Returns with Scene borrowing tasks settled, but retains all remaining scenes.
        // Fixed groups run during BEGIN_UPDATE waits; Variable groups run once per admitted round.
        // Event callbacks must not throw. A platform wait must be woken by Execution's setWake callback.
        [[nodiscard]] Result run(EventPump pump_events = {}, EventWait wait_events = {}) noexcept;

        // Main-thread request. Stops new rounds, then drains accepted work; this is not pause/resume.
        void requestStop() noexcept;

    private:
        friend class EngineContext;
        void prependEngineStageCallback(EngineStageAsyncCallback callback, EEngineStage stage) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::engine
