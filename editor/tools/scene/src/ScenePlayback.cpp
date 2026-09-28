#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/ui/scene/InspectorPane.hpp>
#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>

namespace lux::editor::scene
{
    namespace
    {
        auto playbackError(std::string domain, EEditorError code = EEditorError::INVALID_STATE)
        {
            return lux::cxx::unexpected(EditorFailure{code, std::move(domain)});
        }
    }

    EditorResult<RunId> SceneEditor::Impl::play(std::chrono::nanoseconds fixed_step)
    {
        if (!scene || asset_status_.phase != EAssetEditPhase::IDLE)
            return playbackError("scene.unavailable", EEditorError::BUSY);
        const auto finished = finishEditing();
        if (!finished)
            return lux::cxx::unexpected(finished.error());
        const bool busy =
            (editing_busy || editing().busy()) || !runSettled();
        if (busy)
            return playbackError("run.active", EEditorError::BUSY);
        const bool invalid_step = fixed_step.count() <= 0 || fixed_step > std::chrono::seconds(1);
        if (invalid_step || next_run == UINT64_MAX)
            return playbackError("run.fixed-step", EEditorError::INVALID_ARGUMENT);
        if (!changed_assets.empty())
            return playbackError("run.asset-source", EEditorError::BUSY);
        auto capture = captureSource();
        if (!capture)
            return lux::cxx::unexpected(capture.error());
        run_stop = {};
        auto& execution = editor_context_.execution();
        auto admitted = execution.submit(
            {"Prepare scene playback", "scene"},
            [capture = std::move(*capture),
             cpu = execution.cpu(),
             stop = run_stop.get_token()](process::TaskReporter reporter) mutable noexcept {
                reporter.setPhase("Build scene package");
                return stdexec::then(stdexec::schedule(cpu), PlaybackBuild{std::move(capture), stop});
            },
            [this](process::TTaskResult<lux::scene::ScenePackage, EditorFailure>&& result) noexcept {
                run_prepared_.emplace(lux::editor::detail::taskResult(std::move(result)));
                run_preparation = {};
                completion_work_.request();
            }
        );
        if (!admitted)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::EXECUTION_FAILURE,
                "run.prepare",
                static_cast<std::uint64_t>(admitted.error()),
                {},
                admitted.error()
            });
        run_preparation = std::move(*admitted);
        run_assets = asset_source;
        run_receipt = {};
        run_page_size = 0;
        run_delta = fixed_step;
        single_step = false;
        run_status = {
            .id = {history->id(), next_run++},
            .state = ERunState::PREPARING,
            .captured_state = history->view()->snapshot.current
        };
        return run_status.id;
    }

    EditorResult<void> SceneEditor::Impl::pauseRun(RunId id)
    {
        if (id != run_status.id)
            return playbackError("run.identity", EEditorError::STALE_REQUEST);
        if ((editing_busy || editing().busy()))
            return playbackError("run.interaction", EEditorError::BUSY);
        if (!run_scene || run_status.state != ERunState::RUNNING)
            return playbackError("run.pause");
        const auto paused = runtime_.invalid(*run_scene);
        if (!paused)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::EXECUTION_FAILURE, "run.pause", 0, {}, paused.error()}
            );
        run_status.pause_pending = true;
        single_step = false;
        return {};
    }

    EditorResult<void> SceneEditor::Impl::resumeRun(RunId id)
    {
        if (asset_status_.phase != EAssetEditPhase::IDLE)
            return playbackError("scene.unavailable", EEditorError::BUSY);
        if (id != run_status.id)
            return playbackError("run.identity", EEditorError::STALE_REQUEST);
        if ((editing_busy || editing().busy()))
            return playbackError("run.interaction", EEditorError::BUSY);
        if (!run_scene || run_status.state != ERunState::PAUSED)
            return playbackError("run.resume");
        const auto finished = finishEditing();
        if (!finished)
            return lux::cxx::unexpected(finished.error());
        if (run_history)
        {
            if (inspector_)
            {
                const auto detached = inspector_->content().setTarget(*scene_editing, selection_.object);
                if (!detached)
                    return lux::cxx::unexpected(detached.error());
            }
            const auto closed = run_history->close();
            if (!closed)
                return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.history", 0, {}, closed.error()});
            run_editing.reset();
            run_history.reset();
        }
        const auto resumed = runtime_.valid(*run_scene);
        if (!resumed)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::EXECUTION_FAILURE, "run.resume", 0, {}, resumed.error()}
            );
        run_status.state = ERunState::RUNNING;
        run_status.pause_pending = false;
        single_step = false;
        return {};
    }

    EditorResult<void> SceneEditor::Impl::stepRun(RunId id)
    {
        if (this->single_step)
            return playbackError("run.step", EEditorError::BUSY);
        auto resumed = resumeRun(id);
        if (!resumed)
            return resumed;
        this->single_step = true;
        step_baseline_ = std::visit(
            [](const auto& clock) { return clock.snapshot().step_index; },
            runtime_.getClock(*run_scene)->get()
        );
        return {};
    }

    EditorResult<void> SceneEditor::Impl::stopRun(RunId id)
    {
        if (!scene)
            return {};
        if (id != run_status.id)
            return playbackError("run.identity", EEditorError::STALE_REQUEST);
        if ((editing_busy || editing().busy()))
            return playbackError("run.interaction", EEditorError::BUSY);
        const auto finished = finishEditing();
        if (!finished)
            return lux::cxx::unexpected(finished.error());
        if (!runSettled())
        {
            run_status.state = ERunState::STOPPING;
            run_stop.request_stop();
            if (run_scene)
                static_cast<void>(runtime_.invalid(*run_scene));
        }
        return {};
    }

    RunStatus SceneEditor::Impl::runStatus() const
    {
        return this->run_status;
    }
    double SceneEditor::Impl::runCoordinatePageSize() const noexcept
    {
        return this->run_page_size;
    }

    void SceneEditor::Impl::observePlaybackRender()
    {
        const auto resource = run_receipt.status();
        run_status.render_scene = resource.scene;
        if (run_status.result && !resource.failure.ok())
        {
            run_status.result = lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "run.render", resource.request, {}, resource.failure}
            );
            run_status.failed_phase = resource.scene.isValid() ? ERunPhase::PUBLICATION : ERunPhase::STARTUP;
            run_status.state = ERunState::STOPPING;
            run_stop.request_stop();
            if (run_scene)
                static_cast<void>(runtime_.invalid(*run_scene));
        }
        if (run_scene)
        {
            if (const auto* render = renderFor(run_scene))
            {
                const auto facts = render->transport;
                run_status.published_updates = facts.published;
                run_status.forwarded_updates = facts.forwarded;
                run_status.retired_updates = facts.retired_unforwarded;
                run_status.backpressure_count = facts.backpressured;
                run_status.pending_updates = facts.pending;
                run_status.update_high_water = facts.high_water;
                const auto* assets = lux::scene::RenderAssets::find(readRegistry(*run_scene), render->system);
                run_status.retained_resources = assets->statuses().size();
            }
        }
    }

    void SceneEditor::Impl::failPlayback(EditorFailure error, ERunPhase phase)
    {
        if (run_status.result)
        {
            run_status.result = lux::cxx::unexpected(std::move(error));
            run_status.failed_phase = phase;
        }
        run_status.state = ERunState::STOPPING;
        run_stop.request_stop();
        if (run_scene)
            static_cast<void>(runtime_.invalid(*run_scene));
    }

    void SceneEditor::Impl::observePlayback()
    {
        if (!run_scene || run_status.state == ERunState::STOPPING)
            return;
        const auto& snapshot = progress(*run_scene);
        run_status.steps = snapshot.time.step_index;
        run_status.elapsed = snapshot.time.elapsed;
        run_status
            .completed = {snapshot.simulation_completed, snapshot.stable_completed, snapshot.publication_completed};
        run_status.simulation_work = snapshot.active_work;
        run_status.longest_advance = snapshot.longest_call;
        run_status.publication_wait = snapshot.publication_wait;
        if (!snapshot.result)
        {
            ERunPhase phase{ERunPhase::NONE};
            switch (snapshot.result.error().phase)
            {
            case lux::scene::ESceneDrivePhase::SIMULATION:
                phase = ERunPhase::SIMULATION;
                break;
            case lux::scene::ESceneDrivePhase::SYNCHRONIZATION:
                phase = ERunPhase::SYNCHRONIZATION;
                break;
            case lux::scene::ESceneDrivePhase::MAINTENANCE:
                phase = ERunPhase::MAINTENANCE;
                break;
            case lux::scene::ESceneDrivePhase::STABLE:
                phase = ERunPhase::STABLE;
                break;
            case lux::scene::ESceneDrivePhase::PUBLICATION:
                phase = ERunPhase::PUBLICATION;
                break;
            default:
                break;
            }
            std::visit(
                [&](const auto& cause) {
                    failPlayback({EEditorError::EXECUTION_FAILURE, "run.drive", 0, {}, cause}, phase);
                },
                snapshot.result.error().cause
            );
        }
        else
        {
            if (single_step && snapshot.time.step_index > step_baseline_)
            {
                static_cast<void>(runtime_.invalid(*run_scene));
                run_status.pause_pending = true;
            }
        }
        observePlaybackRender();
    }

    void SceneEditor::Impl::beginPauseEditing()
    {
        const bool has_pause_request = run_status.state == ERunState::RUNNING && run_status.pause_pending;
        if (!run_scene || !has_pause_request)
            return;
        if (run_status.result && safe(*run_scene))
        {
            auto history = editing::EditHistory::create({kHistoryLimits, {}});
            if (!history)
                failPlayback({EEditorError::SOURCE_FAILURE, "run.history", 0, {}, history.error()});
            else
            {
                run_history = std::move(*history);
                run_editing = std::make_unique<SceneEditing>(
                    runtime_,
                    *run_scene,
                    editor_context_.sceneRegistrations().components,
                    *run_history,
                    &editor_context_.project()
                );
                run_editing->admission = [this] { return checkEditAdmission(); };
                run_editing->changed = [this](const ComponentNotice& notice) {
                    lux::editor::detail::reportSignalDelivery(
                        editor->emit(editor->componentChanged, notice),
                        "componentChanged"
                    );
                };
                run_status.state = ERunState::PAUSED;
                run_status.pause_pending = false;
                single_step = false;
            }
        }
    }

    void SceneEditor::Impl::adoptPlayback()
    {
        if (run_prepared_)
        {
            auto result = std::move(*run_prepared_);
            run_prepared_.reset();
            if (run_status.state != ERunState::STOPPING)
            {
                if (!result)
                    failPlayback(std::move(result.error()));
                else
                {
                    run_source = std::make_unique<lux::scene::ScenePackage>(std::move(*result));
                    const auto clock = lux::scene::FixedStepClock::create(run_delta);
                    auto created = detail::instantiateScenePackage(
                        runtime_,
                        *run_source,
                        editor_context_.sceneRegistrations(),
                        editor_context_.project().tasks(),
                        editor_context_.renderRuntime(),
                        editor_context_.renderResources(),
                        run_assets,

                        false,
                        *clock
                    );
                    if (!created)
                        failPlayback(std::move(created.error()));
                    else
                    {
                        run_scene = *created;
                        static_cast<void>(runtime_.valid(*run_scene));
                        observeRun();
                        if (auto* render = renderFor(run_scene))
                        {
                            run_receipt = editor_context_.renderResources().sceneReceipt(render->resource);
                            run_page_size = render->coordinate_page_size;
                        }
                        run_status.state = ERunState::RUNNING;
                    }
                }
            }
        }
    }

    void SceneEditor::Impl::updatePlayback()
    {
        if (runSettled())
            return;

        observePlaybackRender();
        if (run_status.state != ERunState::STOPPING || editing().active())
            return;
        // Each view owner revokes its request before the instance is released.
        // Closed elements only keep their resource receipt, so GPU retirement may continue independently.
        if (run_scene)
        {
            const auto requests = readRegistry(*run_scene).view<const lux::scene::RenderViewRequest>();
            const bool has_live_views = std::ranges::any_of(requests, [&](auto entity) {
                const auto& request = requests.get<const lux::scene::RenderViewRequest>(entity);
                return request.destroy_entity_on_stop && !request.stop.stop_requested();
            });
            if (has_live_views)
                return;
        }
        if (run_history)
        {
            // Generated controls borrow run_editing. End that borrow before releasing the pause history.
            if (inspector_)
            {
                const auto detached = inspector_->content().setTarget(*scene_editing, selection_.object);
                if (!detached)
                {
                    failure = detached.error();
                    return;
                }
            }
            auto closed = run_history->close();
            if (!closed)
                return;
            run_editing.reset();
            run_history.reset();
        }
        if (run_scene)
        {
            run_status.retired_updates += run_status.pending_updates;
            run_status.pending_updates = 0;
            run_connections.clear();
            destroyScene(run_scene);
        }
        if (run_preparation)
            return;
        observePlaybackRender();
        if (run_receipt.status().state != lux::scene::ESceneResourceState::RETIRED)
            return;
        run_source.reset();
        run_assets = {};
        run_status.retained_resources = 0;
        run_status.pause_pending = false;
        run_status.state = run_status.result ? ERunState::FINISHED : ERunState::FAILED;
    }
}

namespace lux::editor::scene
{
    EditorResult<RunId> SceneEditor::play(std::chrono::nanoseconds fixed_step)
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
