#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/log/Log.hpp>
#include <algorithm>
#include <thread>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::Impl::requestExit()
    {
        if (phase_ != EApplicationPhase::RUNNING || last_view_)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "exit.phase"});
        auto ids = sessions_.snapshotIds();
        if (!ids)
            return applicationFailure("exit.sessions", ids.error());
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
                if (closing_)
                    for (const auto& saved : closing_->saves())
                        if (saved.save && std::ranges::find(pending_saves_, *saved.save) == pending_saves_.end())
                            pending_saves_.push_back(*saved.save);
                closing_.reset();
                exit_failure_.reset();
                close_decisions_.clear();
                phase_ = EApplicationPhase::RUNNING;
                return {};
            }
            auto decision =
                std::ranges::find(close_decisions_, *review_content_, &sessions::SessionCloseDecision::content);
            if (decision == close_decisions_.end())
                return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "exit.answer"});
            decision->choice = response->choice == desktop::EReviewChoice::SAVE ? sessions::ECloseChoice::SAVE
                                                                                : sessions::ECloseChoice::DISCARD;
            review_content_.reset();
        }
        for (const auto& decision : close_decisions_)
            if (decision.choice == sessions::ECloseChoice::CANCEL)
            {
                const auto request = next_review_++;
                auto question = desktop::ReviewView::create(
                    messages_.dispatcherRef(),
                    lux::ui::PaneId{"exit-review"},
                    {request,
                     "Unsaved content",
                     close_application_ ? "Save this content before closing the Editor?"
                                        : "Save this content before closing it?",
                     {desktop::EReviewChoice::SAVE, desktop::EReviewChoice::DISCARD, desktop::EReviewChoice::CANCEL}}
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
            const auto content = std::ranges::find(content_views_, view.id, &ContentView::view);
            const bool closes_content =
                content != content_views_.end() && std::ranges::any_of(close_decisions_, [&](const auto& decision) {
                    return decision.content.session == content->session;
                });
            if (close_application_ || closes_content)
                ids.push_back(view.id);
        }
        auto closing_views = desktop_->views().prepareClose(ids);
        if (!closing_views)
        {
            if (closing_views.error().retryable)
                return {};
            return applicationFailure("exit.views.prepare", closing_views.error());
        }
        auto permits = closing_->prepare();
        if (!permits)
        {
            if (permits.error().code == sessions::ESessionFactoryError::BUSY)
                return {};
            return applicationFailure("exit.content.prepare", permits.error());
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
        for (const auto& save : closing_->saves())
            if (save.save)
                pending_saves_.push_back(*save.save);
        closing_.reset();
        std::erase_if(content_views_, [&](const auto& view) { return std::ranges::find(ids, view.view) != ids.end(); });
        close_decisions_.clear();
        if (!close_application_)
        {
            phase_ = EApplicationPhase::RUNNING;
            return {};
        }
        opening_.requestStop();
        project_->requestClose();
        desktop_->presentation().stopFrames();
        phase_ = EApplicationPhase::DRAINING;
        return {};
    }
    EditorResult<void> EditorApplication::Impl::settleOperations()
    {
        std::vector<material::MaterialCompileId> material_in_use;
        std::vector<flowforge::FlowCompileId> flow_in_use;
        if (desktop_)
            for (const auto& record : content_views_)
            {
                auto read_operation = [&](lux::ui::Pane& pane) {
                    if (pane.type() == lux::ui::PaneTypeId{"lux.editor.material"})
                        material_in_use.push_back(static_cast<material::MaterialView&>(pane).compilation());
                    else if (pane.type() == lux::ui::PaneTypeId{"lux.editor.flowforge"})
                        flow_in_use.push_back(static_cast<flowforge::FlowView&>(pane).compilation());
                };
                auto used = desktop_->views().withView(record.view, read_operation);
                if (!used && used.error() != views::EViewError::INVALID_ID)
                    return applicationFailure("compilation.view", used.error());
            }
        auto materials = material_compilation_.snapshotIds();
        if (!materials)
            return applicationFailure("material.operations", materials.error());
        for (auto id : *materials)
            if (std::ranges::find(material_in_use, id) == material_in_use.end())
            {
                auto operation = material_compilation_.operation(id);
                if (!operation)
                    return applicationFailure("material.operation", operation.error());
                if (!operation->get().ready())
                    continue;
                auto acknowledged = material_compilation_.acknowledge(id);
                if (!acknowledged)
                    return applicationFailure("material.acknowledge", acknowledged.error());
            }
        auto flows = flow_compilation_.snapshotIds();
        if (!flows)
            return applicationFailure("flow.operations", flows.error());
        for (auto id : *flows)
            if (std::ranges::find(flow_in_use, id) == flow_in_use.end())
            {
                auto operation = flow_compilation_.operation(id);
                if (!operation)
                    return applicationFailure("flow.operation", operation.error());
                if (!operation->get().ready())
                    continue;
                auto acknowledged = flow_compilation_.acknowledge(id);
                if (!acknowledged)
                    return applicationFailure("flow.acknowledge", acknowledged.error());
            }
        for (auto i = pending_saves_.begin(); i != pending_saves_.end();)
        {
            const auto status = saves_.status(*i);
            if (!status)
                return applicationFailure("save.status", status.error());
            if (status->stage != persistence::ESaveStage::TERMINAL)
            {
                ++i;
                continue;
            }
            // Acknowledgement never changes disk publication or the adopted checkpoint.
            const auto acknowledged = saves_.acknowledge(*i);
            if (!acknowledged)
                return applicationFailure("save.acknowledge", acknowledged.error());
            i = pending_saves_.erase(i);
        }
        if (phase_ == EApplicationPhase::DRAINING && materials->empty() && flows->empty() && pending_saves_.empty() &&
            opening_.settled())
        {
            auto closed = project_->advanceClose();
            if (!closed)
                return cxx::unexpected(closed.error());
            if (*closed)
            {
                // This is the actual UI retirement boundary; Engine/renderer/window remain alive.
                desktop_.reset();
                if (engine_->renderContext()->resources().empty())
                    phase_ = EApplicationPhase::RELEASED;
            }
        }
        return {};
    }
    EditorResult<void> EditorApplication::Impl::update()
    {
        if (auto ready = admission(); !ready)
            return ready;
        Dispatch scope{dispatching_};
        if (window_)
        {
            window::LuxWindow::pollEvents();
            input_.sample(*window_);
            if (desktop_)
                if (auto fed = desktop_->feedInput(input_.snapshot()); !fed)
                    return applicationFailure("desktop.input", fed.error());
        }
        if (auto completed = engine_->execution().collectCompletions(); !completed)
            return applicationFailure("execution.collect", completed.error());
        if (auto events = engine_->execution().dispatchTaskEvents(); !events)
            return applicationFailure("execution.events", events.error());
        (void)task_monitor_.dispatchChanges();
        project_->dispatchEvents();
        (void)messages_.dispatchPending();
        saves_.adoptCompletions();
        if (auto submitted = save_execution_.submitReady(); !submitted)
            return applicationFailure("save.submit", submitted.error());
        if (phase_ == EApplicationPhase::RUNNING)
        {
            auto requests = std::exchange(open_intents_, {});
            open_intents_.reserve(64);
            for (auto reference : requests)
                if (auto opened = open(reference); !opened)
                    log::error("application.open", "{}", opened.error().domain);
            if (auto received = receiveOpenResults(); !received)
                return received;
        }
        else if (phase_ == EApplicationPhase::DRAINING)
            if (auto received = opening_.update(); !received)
                return applicationFailure("open.drain", received.error());
        for (auto& content : content_views_)
            if (content.preview)
                content.preview->update();
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
                return applicationFailure("desktop.update", drawn.error());
            for (auto& completion : desktop_->commands()->takeCompletions())
            {
                if (!completion.result)
                    log::error("application.command", "{}", completion.result.error().domain);
                else if (const auto* admitted = std::get_if<commands::AcceptedOperation>(&*completion.result);
                         admitted && admitted->kind == "save")
                    pending_saves_.push_back({admitted->value});
            }
        }
        if (phase_ == EApplicationPhase::RUNNING)
            if (auto closed = receiveViewClose(); !closed)
                log::error("application.close-view", "{}", closed.error().domain);
        if (auto reviewed = reviewClose(); !reviewed)
        {
            // A failed preflight never falls through to destruction. The frozen error needs a user decision;
            // maintenance and already accepted completions continue while the modal is displayed.
            exit_failure_ = std::move(reviewed.error());
            log::error("application.exit", "{}", exit_failure_->domain);
        }
        const auto driven = engine_->sceneRuntime().driveFrame();
        if (!driven)
            return applicationFailure("scene.drive", driven.error());
        if (!driven->empty())
            return applicationFailure("scene.execution", *driven);
        if (auto maintained = runs_.update(); !maintained)
            return applicationFailure("run.receive", maintained.error());
        projections_.collectReleased();
        return settleOperations();
    }
    EditorResult<void> EditorApplication::exec()
    {
        while (impl_->phase_ != EApplicationPhase::RELEASED)
        {
            auto updated = impl_->update();
            if (!updated)
                return updated;
            if (impl_->window_)
                window::LuxWindow::waitEvents(0.001);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {};
    }
}
