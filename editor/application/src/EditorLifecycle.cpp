#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/log/Log.hpp>
#include <algorithm>
#include <thread>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::Impl::requestExit()
    {
        if (phase_ != EApplicationPhase::RUNNING || last_view_ || save_question_ || reload_question_)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "exit.phase"});
        auto ids = sessions_.snapshotIds();
        if (!ids)
            return applicationFailure("exit.sessions", ids.error());
        if (!content_saving_->hasCapacity(ids->size()))
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "exit.save-results"});
        std::vector<sessions::SessionCloseDecision> decisions;
        decisions.reserve(ids->size());
        for (auto id : *ids)
        {
            auto info = sessions_.describe(id);
            if (!info)
                return applicationFailure("exit.content", info.error());
            decisions.push_back(
                {info->current, info->dirty ? sessions::ECloseChoice::CANCEL : sessions::ECloseChoice::DISCARD}
            );
        }
        close_run_decisions_.clear();
        close_destinations_.clear();
        close_application_ = true;
        close_decisions_ = std::move(decisions);
        phase_ = EApplicationPhase::REVIEWING;
        return {};
    }
    EditorResult<void> EditorApplication::Impl::reviewClose()
    {
        if (phase_ != EApplicationPhase::REVIEWING && phase_ != EApplicationPhase::COMMITTING_EXIT)
            return {};
        if (exit_failure_ && !review_)
        {
            auto question = desktop::ReviewView::create(
                messages_.dispatcherRef(),
                lux::ui::PaneId{"exit-review"},
                {next_review_++,
                 "Close could not complete",
                 exit_failure_->domain + "\n" + exit_failure_->message +
                     "\nContent and completed file publications have been retained. Cancel to return to the Editor.",
                 {desktop::EReviewChoice::CANCEL}}
            );
            if (!question)
                return applicationFailure("exit.error", question.error());
            views::DetachedView candidate{contracts::CodeLease::builtin(), std::move(*question)};
            auto shown = adopt(candidate, "exit-review");
            if (!shown)
                return cxx::unexpected(shown.error());
            review_ = *shown;
            return {};
        }
        if (review_)
        {
            std::optional<desktop::ReviewAnswer> response;
            auto read_response = [&](lux::ui::Pane& pane) {
                if (pane.type() == lux::ui::PaneTypeId{"lux.editor.review"})
                    response = static_cast<desktop::ReviewView&>(pane).response();
            };
            auto borrowed = desktop_->views().withView(*review_, read_response);
            if (!borrowed)
                return applicationFailure("exit.review", borrowed.error());
            if (!response)
                return {};
            if (response->choice == desktop::EReviewChoice::SAVE && review_content_)
            {
                auto info = sessions_.describe(review_content_->session);
                if (!info)
                    return applicationFailure("close.save.source", info.error());
                if (!info->binding)
                {
                    auto destination = content_saving_->prepare(
                        *review_content_,
                        persistence::ESaveMode::SAVE_AS,
                        response->text
                    );
                    if (!destination)
                    {
                        const auto* session_error = std::any_cast<sessions::ESessionError>(&destination.error().cause);
                        if (destination.error().code == EEditorError::BUSY ||
                            (session_error && *session_error == sessions::ESessionError::BUSY))
                            return {};
                        auto reject = [&](lux::ui::Pane& pane) {
                            static_cast<desktop::ReviewView&>(pane).rejectAnswer(
                                destination.error().domain + ": " + destination.error().message +
                                "\nChoose a valid unused source path, or cancel closing."
                            );
                        };
                        auto shown = desktop_->views().withView(*review_, reject);
                        if (!shown)
                            return applicationFailure("close.save.destination", shown.error());
                        return {};
                    }
                    auto decision =
                        std::ranges::find(close_decisions_, *review_content_, &sessions::SessionCloseDecision::content);
                    if (decision == close_decisions_.end())
                        return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "close.save.decision"});
                    decision->destination = std::move(destination->request.destination);
                    decision->destination_asset = destination->request.asset;
                    close_destinations_.push_back(std::move(destination->asset));
                }
            }
            auto closed = desktop_->views().prepareClose(std::span{&*review_, 1});
            if (!closed)
                return applicationFailure("exit.review.close", closed.error());
            auto committed = desktop_->views().commit(*closed);
            if (!committed)
                return applicationFailure("exit.review.commit", committed.error());
            review_.reset();
            if (response->choice == desktop::EReviewChoice::CANCEL)
            {
                review_content_.reset();
                review_run_.reset();
                close_run_decisions_.clear();
                if (closing_)
                    for (const auto& saved : closing_->saves())
                        if (saved.save)
                        {
                            if (auto remembered = content_saving_->track(*saved.save, close_destinations_); !remembered)
                                return remembered;
                        }
                closing_.reset();
                exit_failure_.reset();
                close_decisions_.clear();
                phase_ = EApplicationPhase::RUNNING;
                return {};
            }
            if (review_run_)
            {
                const auto decision = std::ranges::find(close_run_decisions_, *review_run_, &RunCloseDecision::run);
                if (decision == close_run_decisions_.end())
                    return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "close.run.answer"});
                decision->choice = response->choice;
                review_run_.reset();
            }
            else
            {
                auto decision =
                    std::ranges::find(close_decisions_, *review_content_, &sessions::SessionCloseDecision::content);
                if (decision == close_decisions_.end())
                    return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "exit.answer"});
                decision->choice = response->choice == desktop::EReviewChoice::SAVE ? sessions::ECloseChoice::SAVE
                                                                                    : sessions::ECloseChoice::DISCARD;
                review_content_.reset();
            }
        }
        for (const auto& decision : close_decisions_)
            if (decision.choice == sessions::ECloseChoice::CANCEL)
            {
                auto info = sessions_.describe(decision.content.session);
                if (!info)
                    return applicationFailure("close.review.source", info.error());
                const auto request = next_review_++;
                auto question = desktop::ReviewView::create(
                    messages_.dispatcherRef(),
                    lux::ui::PaneId{"exit-review"},
                    {request,
                     "Unsaved content",
                     close_application_ ? "Save this content before closing the Editor?"
                                        : "Save this content before closing it?",
                     {desktop::EReviewChoice::SAVE, desktop::EReviewChoice::DISCARD, desktop::EReviewChoice::CANCEL},
                     info->binding ? std::nullopt : std::optional<std::string>{"Project-relative source path"},
                     "Content/Untitled.source"}
                );
                if (!question)
                    return applicationFailure("exit.question", question.error());
                views::DetachedView candidate{contracts::CodeLease::builtin(), std::move(*question)};
                auto adopted = adopt(candidate, "exit-review");
                if (!adopted)
                    return cxx::unexpected(adopted.error());
                review_ = *adopted;
                review_content_ = decision.content;
                return {};
            }
        for (const auto& decision : close_run_decisions_)
            if (!decision.choice)
            {
                auto question = desktop::ReviewView::create(
                    messages_.dispatcherRef(),
                    lux::ui::PaneId{"exit-review"},
                    {next_review_++,
                     "Frozen Run still exists",
                     "The Run owns a frozen capture. Keep it independently of the author content, or stop it?",
                     {desktop::EReviewChoice::KEEP_RUN, desktop::EReviewChoice::STOP_RUN, desktop::EReviewChoice::CANCEL
                     }}
                );
                if (!question)
                    return applicationFailure("close.run.question", question.error());
                views::DetachedView candidate{contracts::CodeLease::builtin(), std::move(*question)};
                auto shown = adopt(candidate, "exit-review");
                if (!shown)
                    return cxx::unexpected(shown.error());
                review_ = *shown;
                review_run_ = decision.run;
                return {};
            }
        if (!closing_)
        {
            auto close = sessions::CloseSessionsOperation::begin(sessions_, saves_, close_decisions_);
            if (!close)
                return applicationFailure("exit.prepare", close.error());
            closing_ = std::make_unique<sessions::CloseSessionsOperation>(std::move(*close));
        }
        auto views = desktop_->views().describeAll();
        if (!views)
            return applicationFailure("exit.views", views.error());
        std::vector<views::ViewId> ids;
        ids.reserve(views->size());
        for (const auto& view : *views)
        {
            const bool closes_content = std::ranges::any_of(close_decisions_, [&](const auto& decision) {
                return std::ranges::find(view.content.sessions, decision.content.session) != view.content.sessions.end();
            });
            const bool stops_run = std::ranges::any_of(run_presentations_, [&](const auto& run) {
                return run.run && std::ranges::find(run.views, view.id) != run.views.end() &&
                    std::ranges::any_of(close_run_decisions_, [&](const auto& decision) {
                        return decision.run == *run.run && decision.choice == desktop::EReviewChoice::STOP_RUN;
                    });
            });
            if (close_application_ || closes_content || stops_run)
                ids.push_back(view.id);
        }
        // Resolve every already accepted publication before the irreversible window handoff.
        // Unknown needs an explicit user reconciliation while the Results view is still available.
        const auto ready_save = [&](persistence::SaveId id) -> EditorResult<bool> {
            auto report = std::ranges::find(content_saving_->reports(), id, &ProjectSaveReport::id);
            if (report != content_saving_->reports().end() && report->result)
                return true;
            auto status = saves_.status(id);
            if (!status)
                return applicationFailure("close.save.status", status.error());
            const bool has_catalog_ticket = report != content_saving_->reports().end() && report->catalog_ticket;
            const auto ticket = has_catalog_ticket ? *report->catalog_ticket : status->ticket;
            auto written = writes_.status(ticket);
            if (!written)
                return applicationFailure("close.publication.status", written.error());
            if (written->stage == persistence::EWriteStage::UNKNOWN)
                return cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "close.publication.unknown",
                    0,
                    "Cancel closing and reconcile the retained publication in Operation Results."
                });
            return status->stage == persistence::ESaveStage::TERMINAL &&
                   (report == content_saving_->reports().end() || report->result.has_value());
        };
        for (const auto& artifact : artifacts_)
        {
            if (artifact.terminal())
                continue;
            auto ticket = artifact.operation ? artifact.operation->ticket() : std::nullopt;
            if (ticket)
            {
                auto status = writes_.status(*ticket);
                if (!status)
                    return applicationFailure("close.artifact.status", status.error());
                if (status->stage == persistence::EWriteStage::UNKNOWN)
                    return cxx::unexpected(EditorFailure{
                        EEditorError::SOURCE_FAILURE,
                        "close.artifact.unknown",
                        0,
                        "Cancel closing and reconcile the retained publication in Operation Results."
                    });
            }
            return {}; // Keep the desktop available until accepted artifact and catalog publications settle.
        }
        for (auto id : content_saving_->pending())
        {
            auto ready = ready_save(id);
            if (!ready)
                return cxx::unexpected(ready.error());
            if (!*ready)
                return {};
        }
        for (const auto& entry : closing_->saves())
            if (entry.save)
            {
                auto ready = ready_save(*entry.save);
                if (!ready)
                    return cxx::unexpected(ready.error());
                if (!*ready)
                    return {};
            }
        if (close_application_)
            for (const auto& publication : workspace_publications_)
                if (!publication.result)
                {
                    auto status = writes_.status(publication.ticket);
                    if (!status)
                        return applicationFailure("exit.workspace.status", status.error());
                    if (status->stage == persistence::EWriteStage::UNKNOWN)
                        return cxx::unexpected(EditorFailure{
                            EEditorError::BUSY,
                            "exit.workspace.unknown",
                            0,
                            "Reconcile the pending workspace publication before exiting."
                        });
                    return {};
                }
        if (close_application_)
            if (auto operation = importer_->currentRequest())
            {
                auto state = importer_->status(*operation);
                if (!state)
                    return cxx::unexpected(state.error());
                if (auto* error = std::get_if<EditorFailure>(&*state))
                    return cxx::unexpected(*error);
                if (std::holds_alternative<assets::ModelImportPending>(*state))
                    return {}; // Keep import controls until accepted publication is settled.
            }
        if (close_application_ && !plugin_saving_->settled())
        {
            if (const auto* failure = std::get_if<EditorFailure>(plugin_saving_->status()))
                return cxx::unexpected(*failure);
            return {};
        }
        if (close_application_ && (project_launch_ || (project_creation_ && project_creation_->progress().pending)))
            return {};
        auto closing_views = desktop_->views().prepareClose(ids);
        if (!closing_views)
        {
            if (closing_views.error().retryable)
                return {};
            return applicationFailure("exit.views.prepare", closing_views.error());
        }
        auto permits = closing_->prepare();
        for (const auto& entry : closing_->saves())
            if (entry.save)
            {
                if (auto remembered = content_saving_->track(*entry.save, close_destinations_); !remembered)
                    return remembered;
            }
        if (!permits)
        {
            if (permits.error().code == sessions::ESessionFactoryError::BUSY)
                return {};
            return applicationFailure("exit.content.prepare", permits.error());
        }
        for (const auto& entry : closing_->saves())
            if (entry.save)
            {
                const auto report = std::ranges::find(content_saving_->reports(), *entry.save, &ProjectSaveReport::id);
                if (report == content_saving_->reports().end() || !report->result)
                    return {}; // Keep the UI until this source's catalog publication is settled too.
            }
        if (close_application_)
            phase_ = EApplicationPhase::COMMITTING_EXIT;
        auto close_content = [&] {
            // No intervening business callback: Store validates the entire permit set, then reclaims.
            const auto closed = sessions_.close(*permits);
            if (!closed)
                std::terminate(); // A violated prepared-owner contract cannot be reported as a successful exit.
        };
        auto committed = desktop_->views().commitClose(*closing_views, close_content);
        if (!committed)
            return applicationFailure("exit.views.commit", committed.error());
        closing_.reset();
        for (auto& run : run_presentations_)
            std::erase_if(run.views, [&](auto view) { return std::ranges::find(ids, view) != ids.end(); });
        close_decisions_.clear();
        for (const auto& decision : close_run_decisions_)
            if (decision.choice == desktop::EReviewChoice::STOP_RUN)
            {
                auto run = std::ranges::find(run_presentations_, std::optional{decision.run}, &RunPresentation::run);
                if (run != run_presentations_.end())
                    run->stop_requested = true;
            }
        close_run_decisions_.clear();
        if (!close_application_)
        {
            phase_ = EApplicationPhase::RUNNING;
            return {};
        }
        // The irreversible handoff has completed. Failures from now on cannot return to review.
        phase_ = EApplicationPhase::DRAINING;
        opening_.requestStop();
        importer_->requestClose();
        desktop_->presentation().stopFrames();
        return {};
    }
    EditorResult<void> EditorApplication::Impl::settleOperations()
    {
        EditorResult<void> outcome;
        const auto receive = [&](EditorResult<void> result) {
            if (!result && outcome)
                outcome = std::move(result);
        };
        for (auto& reload : reloads_)
            if (reload.operation)
            {
                if (phase_ == EApplicationPhase::DRAINING)
                    reload.operation->cancel();
                reload.operation->update(opening_.find(reload.source.session));
                if (reload.operation->outcome())
                {
                    reload.result = *reload.operation->outcome();
                    reload.operation.reset();
                }
            }
        if (auto received = material_compilation_.collectReleased(); !received)
            receive(applicationFailure("material.release", received.error()));
        if (auto received = flow_compilation_.collectReleased(); !received)
            receive(applicationFailure("flow.release", received.error()));
        receive(content_saving_->update(closing_ ? closing_->saves() : std::span<const sessions::SaveAllEntry>{}));
        receive(settleArtifacts());
        receive(settleWorkspace());
        const bool operations_settled =
            importer_->closeStatus().state == assets::EModelImportCloseState::CLOSED && material_compilation_.empty() &&
            flow_compilation_.empty() && content_saving_->settled() && opening_.settled() && recent_projects_->settled() && !project_launch_ &&
            std::ranges::all_of(workspace_publications_, [](const auto& value) { return value.result.has_value(); }) &&
            std::ranges::all_of(artifacts_, [](const auto& value) { return value.terminal(); }) &&
            std::ranges::none_of(run_presentations_, [](const auto& run) { return bool(run.preparing) || run.run; });
        if (phase_ == EApplicationPhase::DRAINING && operations_settled &&
            std::ranges::none_of(reloads_, [](const auto& reload) { return bool(reload.operation); }) &&
            std::ranges::none_of(model_placements_, [](const auto& model) { return bool(model.operation); }))
        {
            project_->requestClose();
            auto closed = project_->advanceClose();
            if (!closed)
                receive(cxx::unexpected(closed.error()));
            else if (*closed)
            {
                // This is the actual UI retirement boundary; Engine/renderer/window remain alive.
                desktop_.reset();
                if (engine_->renderContext()->resources().empty())
                    phase_ = EApplicationPhase::RELEASED;
            }
        }
        return outcome;
    }
    EditorResult<void> EditorApplication::Impl::update()
    {
        if (auto ready = admission(); !ready)
            return ready;
        Dispatch scope{dispatching_};
        EditorResult<void> outcome;
        const auto receive = [&](EditorResult<void> result) {
            if (!result && outcome)
                outcome = std::move(result);
        };
        if (window_)
        {
            window::LuxWindow::pollEvents();
            input_.sample(*window_);
            if (desktop_)
                if (auto fed = desktop_->feedInput(input_.snapshot()); !fed)
                    receive(applicationFailure("desktop.input", fed.error()));
        }
        // Independent accepted work keeps its completion path even if another owner reports an error.
        if (auto completed = engine_->execution().collectCompletions(); !completed)
            receive(applicationFailure("execution.collect", completed.error()));
        if (auto events = engine_->execution().dispatchTaskEvents(); !events)
            receive(applicationFailure("execution.events", events.error()));
        (void)task_monitor_.dispatchChanges();
        if (environment_.assets.version != project_->catalogRevision())
        {
            auto reads = project_->captureAssetReads();
            if (!reads)
                receive(cxx::unexpected(reads.error()));
            else
            {
                auto next = environment_.assets;
                next.reads = std::move(*reads);
                next.version = project_->catalogRevision();
                environment_.assets = std::move(next);
                environment_.version = project_->catalogRevision();
            }
        }
        project_->dispatchEvents();
        (void)messages_.dispatchPending();
        if (auto result = std::exchange(project_launch_result_, {}))
            receive(std::move(*result));
        receive(recent_projects_->update(phase_ == EApplicationPhase::RUNNING));
        importer_->update();
        if (project_creation_)
            project_creation_->update();
        receive(maintainProjectSettings());
        saves_.adoptCompletions();
        if (auto submitted = save_execution_.submitReady(); !submitted)
            receive(applicationFailure("save.submit", submitted.error()));
        if (phase_ == EApplicationPhase::RUNNING)
        {
            auto requests = std::exchange(open_intents_, {});
            open_intents_.reserve(64);
            for (auto reference : requests)
                if (auto opened = open(reference); !opened)
                    receive(cxx::unexpected(opened.error()));
            receive(receiveOpenResults());
        }
        else if (phase_ == EApplicationPhase::DRAINING)
            if (auto received = opening_.update(); !received)
                receive(applicationFailure("open.drain", received.error()));
        if (desktop_)
        {
            auto frame = config_.offscreen
                             ? std::optional{lux::ui::FrameInfo{
                                   {static_cast<float>(config_.width), static_cast<float>(config_.height)},
                                   1.F / 60.F
                               }}
                             : std::nullopt;
            if (phase_ == EApplicationPhase::DRAINING)
                frame = lux::ui::FrameInfo{};
            if (auto drawn = desktop_->update(frame); !drawn)
                receive(applicationFailure("desktop.update", drawn.error()));
            for (auto& completion : desktop_->commands()->takeCompletions())
            {
                if (!completion.result)
                    receive(applicationFailure("application.command", completion.result.error()));
            }
        }
        if (result_intent_)
        {
            auto applied = receiveResultIntent();
            if (!applied)
                result_failure_ = applied.error();
            else
                result_failure_.reset();
        }
        if (phase_ == EApplicationPhase::RUNNING)
            receive(receiveProjectIntents());
        settleModels();
        receive(receiveSaveAnswer());
        receive(receiveReloadAnswer());
        if (phase_ == EApplicationPhase::RUNNING)
            receive(receiveViewClose());
        if (auto reviewed = reviewClose(); !reviewed)
        {
            // Preflight failure preserves the frozen review and leaves completion/retirement active.
            exit_failure_ = std::move(reviewed.error());
            log::error("application.exit", "{}", exit_failure_->domain);
        }
        const auto driven = engine_->sceneRuntime().driveFrame();
        if (!driven || !driven->empty())
        {
            auto failure = std::make_shared<SceneFailures>();
            failure->code.assign(plugins_.libraries().begin(), plugins_.libraries().end());
            if (!driven)
                failure->values.push_back(driven.error());
            else
                failure->values.assign(driven->begin(), driven->end());
            receive(applicationFailure("scene.execution", std::shared_ptr<const SceneFailures>{std::move(failure)}));
        }
        if (auto maintained = runs_.update(); !maintained)
            receive(applicationFailure("run.receive", maintained.error()));
        receive(maintainRuns());
        projections_.collectReleased();
        receive(settleOperations());
        if (!outcome && !maintenance_failure_)
            maintenance_failure_ = outcome.error();
        return outcome;
    }
    EditorResult<void> EditorApplication::exec()
    {
        while (impl_->phase_ != EApplicationPhase::RELEASED)
        {
            // A domain failure is a retained result, never permission to destroy accepted work.
            if (auto updated = impl_->update(); !updated)
                log::error("application.update", "{}", updated.error().domain);
            if (impl_->window_)
                window::LuxWindow::waitEvents(0.001);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (impl_->maintenance_failure_)
            return cxx::unexpected(*impl_->maintenance_failure_);
        return {};
    }
}
