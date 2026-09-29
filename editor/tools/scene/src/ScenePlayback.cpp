#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/ui/scene/InspectorPane.hpp>
#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>
#include <SceneRunCaptureAccess.hpp>

namespace lux::editor::scene
{
    namespace
    {
        auto playbackError(std::string domain, EEditorError code = EEditorError::INVALID_STATE)
        {
            return lux::cxx::unexpected(EditorFailure{code, std::move(domain)});
        }
        EditorFailure playbackFailure(const RunFailure& failure, std::string domain)
        {
            if (const auto* runtime = std::get_if<lux::scene::SceneRuntimeFailure>(&failure.cause))
                if (const auto* drive = std::get_if<lux::scene::SceneDriveFailure>(&runtime->cause))
                    return std::visit(
                        [&](const auto& cause) {
                            return EditorFailure{EEditorError::EXECUTION_FAILURE, "run.drive", 0, {}, cause};
                        },
                        drive->cause
                    );
            EEditorError code{EEditorError::SOURCE_FAILURE};
            if (const auto* error = std::get_if<ERunError>(&failure.cause))
            {
                switch (*error)
                {
                case ERunError::BUSY:
                case ERunError::NOT_READY:
                case ERunError::CAPACITY:
                    code = EEditorError::BUSY;
                    break;
                case ERunError::INVALID_ID:
                    code = EEditorError::STALE_REQUEST;
                    break;
                case ERunError::INVALID_CONFIGURATION:
                    code = EEditorError::INVALID_ARGUMENT;
                    break;
                case ERunError::CANCELLED:
                    code = EEditorError::CANCELLED;
                    break;
                default:
                    break;
                }
            }
            return {code, std::move(domain), 0, {}, failure};
        }
        ERunPhase playbackPhase(lux::scene::ESceneDrivePhase phase) noexcept
        {
            switch (phase)
            {
            case lux::scene::ESceneDrivePhase::SIMULATION:
                return ERunPhase::SIMULATION;
            case lux::scene::ESceneDrivePhase::SYNCHRONIZATION:
                return ERunPhase::SYNCHRONIZATION;
            case lux::scene::ESceneDrivePhase::MAINTENANCE:
                return ERunPhase::MAINTENANCE;
            case lux::scene::ESceneDrivePhase::STABLE:
                return ERunPhase::STABLE;
            case lux::scene::ESceneDrivePhase::PUBLICATION:
                return ERunPhase::PUBLICATION;
            default:
                return ERunPhase::NONE;
            }
        }
    }

    // P12 UI bridge: capture the existing Registry author, then delegate execution ownership.
    EditorResult<StartRunId> SceneEditor::Impl::play(std::chrono::nanoseconds fixed_step)
    {
        if (!scene || asset_status_.phase != EAssetEditPhase::IDLE)
            return playbackError("scene.unavailable", EEditorError::BUSY);
        const auto finished = finishEditing();
        if (!finished)
            return lux::cxx::unexpected(finished.error());
        if (editing_busy || editing().busy() || !runSettled() || !changed_assets.empty())
            return playbackError("run.active", EEditorError::BUSY);
        if (fixed_step.count() <= 0 || fixed_step > std::chrono::seconds(1))
            return playbackError("run.fixed-step", EEditorError::INVALID_ARGUMENT);
        if (active_run_.valid())
        {
            const auto closed = runs_.acknowledgeStop(active_run_);
            if (!closed)
                return lux::cxx::unexpected(playbackFailure(closed.error(), "run.acknowledge"));
            active_run_ = {};
        }
        auto capture = captureSource();
        if (!capture)
            return lux::cxx::unexpected(capture.error());
        const auto& registrations = editor_context_.sceneRegistrations();
        RunEnvironment environment{
            registrations.components,
            registrations.simulation_systems,
            registrations.scene_systems,
            {registrations.render_bindings.begin(), registrations.render_bindings.end()},
            &editor_context_.renderRuntime(),
            &editor_context_.renderResources(),
            asset_source
        };
        const auto state = history->view()->snapshot.current;
        auto prepared = transition::SceneRunCaptureAccess::prepare(
            run_controller_,
            std::move(*capture),
            {{}, state},
            std::move(environment),
            {fixed_step, viewport_system_}
        );
        if (!prepared)
            return lux::cxx::unexpected(playbackFailure(prepared.error(), "run.prepare"));
        run_start_ = std::move(*prepared);
        cancelling_start_ = false;
        displayed_step_.reset();
        run_status = {.request = run_start_->id(), .state = EPlaybackState::PREPARING, .captured_state = state};
        return run_start_->id();
    }
    EditorResult<void> SceneEditor::Impl::pauseRun(RunId id)
    {
        if (id != active_run_ || !id.valid())
            return playbackError("run.identity", EEditorError::STALE_REQUEST);
        if (editing_busy || editing().busy())
            return playbackError("run.interaction", EEditorError::BUSY);
        const auto paused = runs_.pause(id);
        if (!paused)
            return lux::cxx::unexpected(playbackFailure(paused.error(), "run.pause"));
        run_status.pause_pending = true;
        return {};
    }
    EditorResult<void> SceneEditor::Impl::resumeRun(RunId id)
    {
        if (id != active_run_ || !id.valid())
            return playbackError("run.identity", EEditorError::STALE_REQUEST);
        if (asset_status_.phase != EAssetEditPhase::IDLE || editing_busy || editing().busy() || displayed_step_)
            return playbackError("run.interaction", EEditorError::BUSY);
        const auto finished = finishEditing();
        if (!finished)
            return finished;
        if (inspector_)
        {
            const auto detached = inspector_->content().setTarget(*scene_editing, selection_.object);
            if (!detached)
                return detached;
        }
        const auto resumed = runs_.resume(id);
        if (!resumed)
            return lux::cxx::unexpected(playbackFailure(resumed.error(), "run.resume"));
        run_editing = nullptr;
        run_history = nullptr;
        observePlayback();
        return {};
    }
    EditorResult<void> SceneEditor::Impl::stepRun(RunId id)
    {
        if (id != active_run_ || !id.valid())
            return playbackError("run.identity", EEditorError::STALE_REQUEST);
        if (editing_busy || editing().busy() || displayed_step_)
            return playbackError("run.interaction", EEditorError::BUSY);
        const auto finished = finishEditing();
        if (!finished)
            return finished;
        if (inspector_)
        {
            const auto detached = inspector_->content().setTarget(*scene_editing, selection_.object);
            if (!detached)
                return detached;
        }
        auto ticket = runs_.step(id);
        if (!ticket)
            return lux::cxx::unexpected(playbackFailure(ticket.error(), "run.step"));
        displayed_step_ = *ticket;
        run_editing = nullptr;
        run_history = nullptr;
        run_status.state = EPlaybackState::RUNNING;
        run_status.pause_pending = true;
        return {};
    }
    EditorResult<void> SceneEditor::Impl::cancelRun(StartRunId id)
    {
        if (id != run_status.request || !run_start_)
            return playbackError("run.start.identity", EEditorError::STALE_REQUEST);
        run_start_->cancel();
        cancelling_start_ = true;
        run_status.state = EPlaybackState::STOPPING;
        run_scene.reset();
        return {};
    }
    EditorResult<void> SceneEditor::cancelRun(StartRunId id)
    {
        return impl_->cancelRun(id);
    }
    EditorResult<void> SceneEditor::Impl::stopRun(RunId id)
    {
        if (id != active_run_ || !id.valid())
            return playbackError("run.identity", EEditorError::STALE_REQUEST);
        if (editing_busy || editing().busy())
            return playbackError("run.interaction", EEditorError::BUSY);
        const auto finished = finishEditing();
        if (!finished)
            return finished;
        if (inspector_)
        {
            const auto detached = inspector_->content().setTarget(*scene_editing, selection_.object);
            if (!detached)
                return detached;
        }
        auto stopped = runs_.stop(id);
        if (!stopped)
            return lux::cxx::unexpected(playbackFailure(stopped.error(), "run.stop"));
        run_editing = nullptr;
        run_history = nullptr;
        displayed_step_.reset();
        run_status.state = EPlaybackState::STOPPING;
        run_scene.reset();
        return {};
    }
    RunStatus SceneEditor::Impl::runStatus() const
    {
        return run_status;
    }
    double SceneEditor::Impl::runCoordinatePageSize() const noexcept
    {
        const auto info = runs_.info(active_run_);
        return info ? info->coordinate_page_size : 0;
    }
    void SceneEditor::Impl::observePlayback()
    {
        if (!active_run_.valid())
            return;
        const auto maintained = runs_.update();
        if (!maintained)
        {
            failure = playbackFailure(maintained.error(), "run.update");
            return;
        }
        const auto info = runs_.info(active_run_);
        if (!info)
            return;
        const auto& snapshot = info->progress;
        run_status.id = info->id;
        run_status.steps = snapshot.time.step_index;
        run_status.elapsed = snapshot.time.elapsed;
        run_status
            .completed = {snapshot.simulation_completed, snapshot.stable_completed, snapshot.publication_completed};
        run_status.simulation_work = snapshot.active_work;
        run_status.longest_advance = snapshot.longest_call;
        run_status.publication_wait = snapshot.publication_wait;
        run_status.pause_pending = info->pause_pending;
        if (!info->result)
        {
            run_status.result = lux::cxx::unexpected(playbackFailure(info->result.error(), "run.drive"));
            if (!snapshot.result)
                run_status.failed_phase = playbackPhase(snapshot.result.error().phase);
        }
        run_status.render_scene = info->render_scene;
        run_status.published_updates = info->published_updates;
        run_status.forwarded_updates = info->forwarded_updates;
        run_status.retired_updates = info->retired_updates;
        run_status.backpressure_count = info->backpressure_count;
        run_status.pending_updates = info->pending_updates;
        run_status.update_high_water = info->update_high_water;
        run_status.retained_resources = info->retained_resources;
        switch (info->state)
        {
        case ERunState::RUNNING:
            run_status.state = EPlaybackState::RUNNING;
            break;
        case ERunState::PAUSED:
            run_status.state = info->pause_pending ? EPlaybackState::RUNNING : EPlaybackState::PAUSED;
            break;
        case ERunState::STOPPING:
            run_status.state = EPlaybackState::STOPPING;
            break;
        case ERunState::STOPPED:
            run_status.state = EPlaybackState::FINISHED;
            break;
        case ERunState::FAILED:
            run_status.state = EPlaybackState::FAILED;
            break;
        }
        if (run_structure_seen_ != info->structure_revision)
        {
            run_structure_seen_ = info->structure_revision;
            run_catalog_changed = true;
        }
        if (displayed_step_)
        {
            const auto step = runs_.stepStatus(*displayed_step_);
            if (step && step->state != lux::scene::ESceneStepState::QUEUED &&
                step->state != lux::scene::ESceneStepState::EXECUTING)
            {
                const auto acknowledged = runs_.acknowledgeStep(*displayed_step_);
                if (acknowledged)
                    displayed_step_.reset();
            }
        }
        if (runSettled() || run_status.state == EPlaybackState::STOPPING)
        {
            run_scene.reset();
            run_editing = nullptr;
            run_history = nullptr;
        }
    }
    void SceneEditor::Impl::beginPauseEditing()
    {
        if (run_status.state != EPlaybackState::PAUSED || run_editing)
            return;
        auto editing = runs_.debugEditing(active_run_);
        auto history = runs_.debugHistory(active_run_);
        if (!editing || !history)
            return;
        run_editing = &editing->get();
        run_history = &history->get();
        run_editing->admission = [this] { return checkEditAdmission(); };
        run_editing->acceptsAsset = [this](asset::AssetId id, std::uint32_t magic) {
            const auto* row = editor_context_.project().catalogAsset(id);
            return row && row->magic == magic;
        };
        run_editing->changed = [this](const ComponentNotice& notice) {
            lux::editor::detail::reportSignalDelivery(
                editor->emit(editor->componentChanged, notice),
                "componentChanged"
            );
        };
    }
    void SceneEditor::Impl::adoptPlayback()
    {
        if (!run_start_ || !run_start_->ready())
            return;
        if (cancelling_start_)
        {
            run_start_.reset();
            run_status.state = EPlaybackState::FINISHED;
            return;
        }
        const auto adopted = run_controller_.adopt(*run_start_);
        if (!adopted)
        {
            run_status.result = lux::cxx::unexpected(playbackFailure(adopted.error(), "run.create"));
            run_status.state = EPlaybackState::FAILED;
            run_start_.reset();
            return;
        }
        active_run_ = *adopted;
        run_scene = runs_.info(active_run_)->instance;
        run_selection = {*run_scene, lux::simulation::ecs::NullEntity, 0};
        run_catalog_changed = true;
        run_start_.reset();
        observePlayback();
    }
    void SceneEditor::Impl::updatePlayback()
    {
        if (run_status.state == EPlaybackState::STOPPING && inspector_)
        {
            const auto detached = inspector_->content().setTarget(*scene_editing, selection_.object);
            if (!detached)
                failure = detached.error();
        }
    }
}

namespace lux::editor::scene
{
    EditorResult<StartRunId> SceneEditor::play(std::chrono::nanoseconds fixed_step)
    {
        return impl_->play(std::move(fixed_step));
    }

    EditorResult<void> SceneEditor::pauseRun(RunId id)
    {
        return impl_->pauseRun(std::move(id));
    }

    EditorResult<void> SceneEditor::resumeRun(RunId id)
    {
        return impl_->resumeRun(std::move(id));
    }

    EditorResult<void> SceneEditor::stepRun(RunId id)
    {
        return impl_->stepRun(std::move(id));
    }

    EditorResult<void> SceneEditor::stopRun(RunId id)
    {
        return impl_->stopRun(std::move(id));
    }

    RunStatus SceneEditor::runStatus() const
    {
        return impl_->runStatus();
    }

}
