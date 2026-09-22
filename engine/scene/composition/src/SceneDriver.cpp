#include <lux/engine/scene/SceneDriver.hpp>
#include <lux/engine/scene/detail/SceneInstanceImpl.hpp>

#include <algorithm>
#include <cassert>

namespace lux::scene
{
namespace
{
using Clock = std::chrono::steady_clock;
bool terminal(ESceneDriveState state) noexcept
{
    return state == ESceneDriveState::STOPPED || state == ESceneDriveState::FAILED;
}
} // namespace

SceneDriver::ControlResult SceneDriver::play(SceneInstance &instance) noexcept
{
    auto &d = *instance.impl_;
    if (terminal(d.progress.state) || d.progress.state == ESceneDriveState::STOPPING)
    {
        return lux::cxx::unexpected(ESceneControlError::STOPPED);
    }
    if (instance.simulation().mode() != simulation::ESimulationMode::EVOLUTION)
    {
        return lux::cxx::unexpected(ESceneControlError::WRONG_MODE);
    }
    if (d.advancing || d.progress.step_pending)
    {
        return lux::cxx::unexpected(ESceneControlError::BUSY);
    }
    d.progress.state = ESceneDriveState::RUNNING;
    d.progress.pause_pending = false;
    d.next = {};
    return {};
}

SceneDriver::ControlResult SceneDriver::pause(SceneInstance &instance) noexcept
{
    auto &d = *instance.impl_;
    if (d.progress.state != ESceneDriveState::RUNNING)
    {
        return lux::cxx::unexpected(ESceneControlError::INVALID_STATE);
    }
    d.progress.pause_pending = true;
    return {};
}

SceneDriver::ControlResult SceneDriver::step(SceneInstance &instance) noexcept
{
    auto &d = *instance.impl_;
    if (d.advancing || d.progress.step_pending || d.progress.phase != ESceneDrivePhase::NONE)
    {
        return lux::cxx::unexpected(ESceneControlError::BUSY);
    }
    if (d.progress.state != ESceneDriveState::PAUSED)
    {
        return lux::cxx::unexpected(ESceneControlError::INVALID_STATE);
    }
    if (instance.simulation().mode() != simulation::ESimulationMode::EVOLUTION)
    {
        return lux::cxx::unexpected(ESceneControlError::WRONG_MODE);
    }
    d.progress.step_pending = true;
    d.progress.pause_pending = true;
    d.progress.state = ESceneDriveState::RUNNING;
    return {};
}

void SceneDriver::stop(SceneInstance &instance) noexcept
{
    auto &d = *instance.impl_;
    if (!terminal(d.progress.state))
    {
        d.progress.state = ESceneDriveState::STOPPING;
    }
    d.stop.request_stop();
}

void SceneDriver::invalidate(SceneInstance &instance) noexcept
{
    instance.impl_->invalidated = true;
}

ESceneProgress SceneDriver::advance(SceneInstance &instance, Clock::time_point now, SceneAdvanceBudget &budget) noexcept
{
    auto &d = *instance.impl_;
    // A callback can request a control change, but cannot recursively advance
    // the same Registry while its synchronous graph is executing.
    if (d.advancing)
    {
        return ESceneProgress::PENDING;
    }
    d.advancing = true;
    const auto entered = Clock::now();
    struct Exit final
    {
        SceneInstance::Impl &data;
        Clock::time_point entered;
        ~Exit()
        {
            data.advancing = false;
            data.progress.longest_call =
                std::max(data.progress.longest_call,
                         std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - entered));
        }
    } exit{d, entered};
    const auto fail = [&](SceneDriveFailure failure) {
        if (d.progress.result)
        {
            d.progress.result = lux::cxx::unexpected(std::move(failure));
        }
        d.progress.state = ESceneDriveState::FAILED;
        d.stop.request_stop();
        d.simulation->stop();
    };

    // Maintenance is independent of phase backpressure and pause. Rotate the
    // first hook so a small shared budget cannot starve later systems.
    SceneStageContext context{budget.publications, budget.resource_steps, d.progress.clock.elapsed,
                              d.progress.clock.step_index};
    context.delta = d.progress.clock.delta;
    context.allow_structure = d.progress.phase == ESceneDrivePhase::NONE;
    context.stop = d.stop.get_token();

    const auto count = d.maintenance_hooks.size();
    for (; d.maintenance_visited < count && budget.system_calls; ++d.maintenance_visited)
    {
        auto &hook = d.maintenance_hooks[d.maintenance_cursor];
        d.maintenance_cursor = (d.maintenance_cursor + 1) % count;
        --budget.system_calls;
        auto result = hook.invoke(context);
        if (!result)
        {
            fail({ESceneDrivePhase::MAINTENANCE, std::move(result.error())});
        }
        else
        {
            d.maintenance_pending |= *result == ESceneProgress::PENDING;
        }
    }
    const bool maintenance_complete = d.maintenance_visited == count;
    const bool maintenance_pending = d.maintenance_pending || !maintenance_complete;
    // Keep a completed maintenance round across budget exhaustion. Otherwise
    // a one-call turn repeats maintenance forever and never resumes its phase.
    const auto finish_maintenance = [&] {
        if (maintenance_complete)
        {
            d.maintenance_visited = 0;
            d.maintenance_pending = false;
        }
    };
    d.invalidated |= context.invalidated;
    if (d.progress.state == ESceneDriveState::STOPPING)
    {
        finish_maintenance();
        d.simulation->stop();
        d.progress.state = d.progress.result ? ESceneDriveState::STOPPED : ESceneDriveState::FAILED;
        return ESceneProgress::COMPLETE;
    }
    if (terminal(d.progress.state))
    {
        finish_maintenance();
        return ESceneProgress::COMPLETE;
    }

    const auto advance_hooks = [&](auto &hooks, ESceneDrivePhase phase) -> ESceneProgress {
        while (d.stage_cursor < hooks.size())
        {
            if (!budget.system_calls || (phase == ESceneDrivePhase::PUBLICATION && !budget.publications))
            {
                // No admission opportunity is different from backend backpressure.
                // Preserve the completed maintenance round until publication can
                // actually try; rotating owners must not reset it in lockstep.
                return ESceneProgress::PENDING;
            }
            --budget.system_calls;
            finish_maintenance();
            auto result = hooks[d.stage_cursor].invoke(context);
            if (!result)
            {
                fail({phase, std::move(result.error())});
                return ESceneProgress::PENDING;
            }
            if (d.progress.state == ESceneDriveState::STOPPING || *result == ESceneProgress::PENDING)
            {
                return ESceneProgress::PENDING;
            }
            ++d.stage_cursor;
        }
        d.stage_cursor = 0;
        return ESceneProgress::COMPLETE;
    };
    if (d.progress.phase == ESceneDrivePhase::NONE)
    {
        if (maintenance_pending)
        {
            finish_maintenance();
            return ESceneProgress::PENDING;
        }
        if (d.progress.pause_pending && !d.progress.step_pending)
        {
            d.progress.state = ESceneDriveState::PAUSED;
            d.progress.pause_pending = false;
        }
        const bool evolve = d.progress.state == ESceneDriveState::RUNNING && (d.progress.step_pending || now >= d.next);
        if (!evolve && !d.invalidated)
        {
            finish_maintenance();
            return ESceneProgress::COMPLETE;
        }
        if (!budget.new_steps)
        {
            return ESceneProgress::PENDING;
        }
        --budget.new_steps;
        d.invalidated = false;
        d.refreshing = !evolve;
        d.progress.phase = evolve ? ESceneDrivePhase::SIMULATION : ESceneDrivePhase::DERIVATION;
        const auto started = Clock::now();
        auto result = evolve ? d.simulation->execute(executor_, d.fixed_delta) : d.simulation->refresh(executor_);
        // Even a failed system may have adopted the next time step.
        d.progress.clock = d.simulation->clock().snapshot();
        context.elapsed = d.progress.clock.elapsed;
        context.step = d.progress.clock.step_index;
        context.delta = d.progress.clock.delta;
        const auto completed = Clock::now();
        d.progress.active_work += completed - started;
        if (evolve)
        {
            d.next = std::max(now + d.fixed_delta, now + (completed - started));
        }
        if (!result)
        {
            fail({d.progress.phase, result.error()});
            return ESceneProgress::COMPLETE;
        }
        if (evolve)
        {
            d.progress.simulation_completed = d.progress.clock.step_index;
        }
        if (d.progress.state == ESceneDriveState::STOPPING)
        {
            return ESceneProgress::PENDING;
        }
        d.progress.phase = ESceneDrivePhase::STABLE;
    }
    if (d.progress.phase == ESceneDrivePhase::STABLE)
    {
        if (advance_hooks(d.stable_point_hooks, ESceneDrivePhase::STABLE) == ESceneProgress::PENDING)
        {
            return ESceneProgress::PENDING;
        }
        d.progress.stable_completed = d.progress.clock.step_index;
        d.progress.phase = ESceneDrivePhase::PUBLICATION;
        d.waiting_since = Clock::now();
    }
    if (advance_hooks(d.publication_hooks, ESceneDrivePhase::PUBLICATION) == ESceneProgress::PENDING)
    {
        return ESceneProgress::PENDING;
    }
    d.progress.publication_wait += Clock::now() - d.waiting_since;
    d.progress.publication_completed = d.progress.clock.step_index;
    d.progress.phase = ESceneDrivePhase::NONE;
    if (d.refreshing)
    {
        ++d.progress.refresh_completed;
    }
    d.progress.step_pending = false;
    finish_maintenance();
    if (d.progress.pause_pending)
    {
        d.progress.state = ESceneDriveState::PAUSED;
        d.progress.pause_pending = false;
    }
    return ESceneProgress::COMPLETE;
}
} // namespace lux::scene
