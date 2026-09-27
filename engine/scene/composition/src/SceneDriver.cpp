#include <lux/engine/scene/detail/SceneDriver.hpp>
#include <lux/engine/scene/detail/SceneInstanceImpl.hpp>

#include <algorithm>
#include <limits>

namespace lux::scene
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct DriveCall final
        {
            bool& advancing;
            SceneDriveSnapshot& progress;
            Clock::time_point entered{Clock::now()};

            DriveCall(bool& active, SceneDriveSnapshot& snapshot) noexcept : advancing(active), progress(snapshot)
            {
                advancing = true;
            }
            ~DriveCall()
            {
                advancing = false;
                progress.longest_call = std::max(
                    progress.longest_call,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - entered)
                );
            }
        };

        template <class State> void fail(State& state, SceneDriveFailure failure) noexcept
        {
            if (state.progress.result)
            {
                state.progress.result = lux::cxx::unexpected(std::move(failure));
            }
            state.stop.request_stop();
            state.simulation->stop();
        }
    } // namespace

    SceneDriver::TickResult SceneDriver::tick(SceneInstance& instance, simulation::SimulationDuration elapsed) noexcept
    {
        auto& state = *instance.impl_;
        const auto rejected = [](ESceneDriveError error) -> TickResult {
            return lux::cxx::unexpected(SceneDriveFailure{ESceneDrivePhase::NONE, error});
        };
        if (state.advancing)
            return rejected(ESceneDriveError::REENTRANT);
        if (!state.progress.result)
            return lux::cxx::unexpected(state.progress.result.error());
        if (state.stop.stop_requested())
            return rejected(ESceneDriveError::STOPPED);
        const auto before = state.simulation->time();
        const bool is_time_reversed = elapsed < before.elapsed;
        const bool is_step_overflow = before.step_index == std::numeric_limits<std::uint64_t>::max();
        const bool is_invalid_time = is_time_reversed || is_step_overflow;
        if (is_invalid_time)
            return rejected(ESceneDriveError::INVALID_TIME);
        const bool is_pending = !state.maintenance_ready || state.progress.phase != ESceneDrivePhase::NONE;
        if (is_pending)
            return ESceneTickResult::DEFERRED;
        DriveCall call(state.advancing, state.progress);
        state.progress.phase = ESceneDrivePhase::SIMULATION;
        const auto started = Clock::now();
        auto result = state.simulation->execute(executor_, {elapsed, elapsed - before.elapsed, before.step_index + 1U});
        state.progress.active_work += Clock::now() - started;
        state.progress.time = state.simulation->time();
        if (!result)
        {
            fail(state, {ESceneDrivePhase::SIMULATION, result.error()});
            return lux::cxx::unexpected(state.progress.result.error());
        }
        state.progress.simulation_completed = state.progress.time.step_index;
        state.publication_needed = true;
        state.stage_cursor = 0;
        state.progress.phase = ESceneDrivePhase::SYNCHRONIZATION;
        return ESceneTickResult::EXECUTED;
    }

    ESceneProgress SceneDriver::maintain(SceneInstance& instance) noexcept
    {
        auto& state = *instance.impl_;
        if (state.advancing)
            return ESceneProgress::PENDING;
        DriveCall call(state.advancing, state.progress);
        const auto time = state.simulation->time();
        SceneStageContext context;
        context.elapsed = time.elapsed;
        context.step = time.step_index;
        context.delta = time.delta;
        context.allow_structure = state.progress.phase == ESceneDrivePhase::NONE && !state.stop.stop_requested();
        context.stop = state.stop.get_token();
        bool maintenance_pending{};
        for (auto& hook : state.maintenance_hooks)
        {
            auto result = hook.invoke(context);
            if (!result)
            {
                fail(state, {ESceneDrivePhase::MAINTENANCE, std::move(result.error())});
                context.allow_structure = false;
            }
            else
            {
                maintenance_pending |= *result == ESceneProgress::PENDING;
            }
        }
        state.maintenance_ready = !maintenance_pending;
        state.publication_needed |= context.publication_needed;
        if (state.stop.stop_requested())
        {
            state.simulation->stop();
            state.stage_cursor = 0;
            state.progress.phase = ESceneDrivePhase::NONE;
        }
        return maintenance_pending ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
    }

    ESceneProgress SceneDriver::publish(SceneInstance& instance) noexcept
    {
        auto& state = *instance.impl_;
        if (state.advancing)
            return ESceneProgress::PENDING;
        DriveCall call(state.advancing, state.progress);
        if (state.stop.stop_requested())
            return state.maintenance_ready ? ESceneProgress::COMPLETE : ESceneProgress::PENDING;
        const auto time = state.simulation->time();
        SceneStageContext context;
        context.elapsed = time.elapsed;
        context.step = time.step_index;
        context.delta = time.delta;
        context.stop = state.stop.get_token();
        const auto completion = state.maintenance_ready ? ESceneProgress::COMPLETE : ESceneProgress::PENDING;
        if (state.progress.phase == ESceneDrivePhase::NONE)
        {
            state.stage_cursor = 0;
            state.progress.phase = ESceneDrivePhase::SYNCHRONIZATION;
        }
        const auto advance_hooks = [&](auto& hooks, ESceneDrivePhase phase) {
            while (state.stage_cursor < hooks.size())
            {
                auto result = hooks[state.stage_cursor].invoke(context);
                state.publication_needed |= context.publication_needed;
                if (!result)
                {
                    fail(state, {phase, std::move(result.error())});
                    return ESceneProgress::PENDING;
                }
                if (state.stop.stop_requested() || *result == ESceneProgress::PENDING)
                    return ESceneProgress::PENDING;
                ++state.stage_cursor;
            }
            state.stage_cursor = 0;
            return ESceneProgress::COMPLETE;
        };
        if (state.progress.phase == ESceneDrivePhase::SYNCHRONIZATION)
        {
            context.allow_structure = true;
            if (advance_hooks(state.synchronization_hooks, ESceneDrivePhase::SYNCHRONIZATION) ==
                ESceneProgress::PENDING)
                return ESceneProgress::PENDING;
            if (!state.publication_needed)
            {
                state.progress.phase = ESceneDrivePhase::NONE;
                return completion;
            }
            state.publication_needed = false;
            context.publication_needed = false;
            state.progress.phase = ESceneDrivePhase::STABLE;
        }
        context.allow_structure = false;
        if (state.progress.phase == ESceneDrivePhase::STABLE)
        {
            if (advance_hooks(state.stable_point_hooks, ESceneDrivePhase::STABLE) == ESceneProgress::PENDING)
                return ESceneProgress::PENDING;
            state.progress.stable_completed = time.step_index;
            state.progress.phase = ESceneDrivePhase::PUBLICATION;
            state.waiting_since = Clock::now();
        }
        if (advance_hooks(state.publication_hooks, ESceneDrivePhase::PUBLICATION) == ESceneProgress::PENDING)
            return ESceneProgress::PENDING;
        state.progress.publication_wait += Clock::now() - state.waiting_since;
        state.progress.publication_completed = time.step_index;
        state.progress.phase = ESceneDrivePhase::NONE;
        return completion;
    }
}
