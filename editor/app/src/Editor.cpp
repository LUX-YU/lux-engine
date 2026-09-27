#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <cstdio>
#include <algorithm>

namespace lux::editor
{
    Editor::Impl::Impl() = default;
    Editor::Impl::~Impl()
    {
        menu_removed_.disconnect();
        if (root)
        {
            root->closeInput();
            root->bindWindow(nullptr);
        }
        if (window)
            window->hide(true);
        // Members release tools, Presentation and project resources before EngineContext and the native window.
    }

    Editor::Editor(EditorConfig config, std::unique_ptr<Impl> impl)
        : Root(impl->messages->dispatcherRef()), impl_(std::move(impl))
    {
        impl_->root = this;
        impl_->config_ = std::move(config);
    }

    Editor::~Editor() = default;

    void Editor::Impl::requestExit() noexcept
    {
        if (!exit_requested_)
            exit_intent = true;
    }
    void Editor::Impl::event(object::EventView& event) noexcept
    {
        if (auto* decision = event.getIf<CloseDecision>())
        {
            event.accept();
            if (decision->request != exit_request_.id || !reviewing_exit_)
                return;
            const bool is_participant = std::ranges::any_of(close_targets_, [&](const auto& id) {
                return context->panes().find(id.view()) == decision->participant;
            });
            if (!is_participant)
                return;
            const auto found = std::ranges::find(close_decisions_, decision->participant, &CloseDecision::participant);
            if (found == close_decisions_.end())
                close_decisions_.push_back(*decision);
            else
                *found = *decision;
            close_decisions_pending_ = true;
        }
        else if (auto* request = event.getIf<AssetOpenRequest>())
        {
            event.accept();
            if (!exit_requested_ && !reviewing_exit_)
                asset_requests_.push_back(request->asset);
        }
        else if (auto* request = event.getIf<PaneCloseRequest>())
        {
            event.accept();
            const bool can_close = request->pane && !exit_requested_ && !reviewing_exit_ &&
                                   context->panes().find(request->pane->id().view()) == request->pane;
            if (can_close)
            {
                const auto found = std::ranges::find(close_requests_, request->pane->id(), &CloseIntent::pane);
                if (found == close_requests_.end())
                    close_requests_.push_back({request->pane->id(), request->purpose});
            }
        }
        else if (auto* request = event.getIf<lux::ui::MenuRequest>())
        {
            event.accept();
            receiveMenu(*request);
        }
        else if (auto* request = event.getIf<WorkspaceRequest>())
        {
            event.accept();
            if (request->action == EWorkspaceAction::STATUS)
                workspaceRequest(*request);
            else if (workspace_pending_ || workspace_intent_ || reviewing_exit_)
                request->result = lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace"});
            else
            {
                workspace_intent_ = *request;
                request->pending = true;
            }
        }
        else if (auto* command = event.getIf<lux::ui::Command>())
        {
            if (command->phase == lux::ui::ECommandPhase::QUERY)
            {
                applicationCommand(*command);
                root_command_ = command->result != lux::ui::ECommandDispatchResult::NOT_FOUND;
                if (root_command_)
                    event.accept();
            }
            else if (active_command_)
            {
                root_command_ = true;
                event.accept();
            }
            else
            {
                MenuCall call;
                call.command = lux::ui::CommandId{std::string(command->id.name())};
                // A direct product event uses the current handler; menu selections have their own frozen revision.
                call.commands_revision = 0;
                menu_requests_.push_back(std::move(call));
                event.accept();
            }
        }
    }
    void Editor::Impl::fail(EditorFailure failure)
    {
        if (outcome_)
        {
            std::fprintf(stderr, "[editor.failure] %s: %s\n", failure.domain.c_str(), failure.message.c_str());
            outcome_ = lux::cxx::unexpected(std::move(failure));
        }
        exit_requested_ = true;
    }
    void Editor::Impl::beginExitReview()
    {
        if (reviewing_exit_ || exit_requested_)
            return;
        if (exit_request_.id == UINT64_MAX)
        {
            fail({EEditorError::CAPACITY, "editor.close.identity"});
            return;
        }
        exit_request_ = {
            exit_request_.id + 1,
            ECloseAction::REVIEW,
            close_intent_.isValid() ? close_purpose_ : EClosePurpose::EXIT
        };
        reviewing_exit_ = true;
        close_decisions_.clear();
        close_targets_.clear();
        for (const auto& pane : context->panes().panes())
            if (!close_intent_.isValid() || pane->id() == close_intent_)
                close_targets_.push_back(pane->id());
        close_intent_ = {};
        context->panes().setFrozen(true);
        // Fixed request participants, including hidden windows.
        for (const auto& id : close_targets_)
        {
            auto* pane = context->panes().find(id.view());
            auto request = exit_request_;
            if (!object::sendEvent(*pane, request))
                close_decisions_.push_back({request.id, pane, ECloseDecision::READY});
        }
        close_decisions_pending_ = true;
    }
    void Editor::Impl::cancelExit()
    {
        for (const auto& id : close_targets_)
        {
            CloseRequest request{exit_request_.id, ECloseAction::CANCEL, exit_request_.purpose};
            if (auto* pane = context->panes().find(id.view()))
                static_cast<void>(object::sendEvent(*pane, request));
        }
        context->panes().setFrozen(false);
        reviewing_exit_ = false;
        close_targets_.clear();
        close_decisions_.clear();
        exit_request_.action = ECloseAction::CANCEL;
        cancelNativeClose();
    }
    void Editor::Impl::commitExit()
    {
        exit_requested_ = true;
        reviewing_exit_ = false;
        window->hide(true);
        presentation->stopFrames();
        // READY is permission to leave the loop. Destruction releases resources.
    }
    void Editor::Impl::applyCloseDecisions()
    {
        if (!reviewing_exit_)
            return;
        for (const auto& decision : close_decisions_)
        {
            const bool was_cancelled = decision.decision == ECloseDecision::CANCELLED;
            const bool has_failed = decision.decision == ECloseDecision::FAILED;
            if (was_cancelled || has_failed)
            {
                if (decision.failure)
                    std::fprintf(
                        stderr,
                        "[editor.close] %s: %s\n",
                        decision.failure->domain.c_str(),
                        decision.failure->message.c_str()
                    );
                cancelExit();
                return;
            }
        }
        if (close_decisions_.size() != close_targets_.size())
            return;
        if (exit_request_.purpose == EClosePurpose::EXIT)
        {
            commitExit();
            return;
        }
        context->panes().setFrozen(false);
        for (const auto& id : close_targets_)
        {
            if (exit_request_.purpose == EClosePurpose::DESTROY)
                static_cast<void>(context->panes().erase(id.view()));
            else if (auto* pane = context->panes().find(id.view()))
            {
                CloseRequest cancelled{exit_request_.id, ECloseAction::CANCEL, EClosePurpose::HIDE};
                static_cast<void>(object::sendEvent(*pane, cancelled));
                pane->setVisible(false);
            }
        }
        reviewing_exit_ = false;
        close_targets_.clear();
        close_decisions_.clear();
    }
    void Editor::Impl::handleRequests()
    {
        if (std::exchange(exit_intent, false))
            beginExitReview();
        if (std::exchange(close_decisions_pending_, false))
            applyCloseDecisions();
        if (exit_requested_ || reviewing_exit_)
            return;
        applyWorkspaceResult();
        if (workspace_intent_)
        {
            auto request = std::move(*workspace_intent_);
            workspace_intent_.reset();
            workspaceRequest(request);
            if (!request.result)
                reportMenuFailure(request.result.error());
        }
        updateSaveAll();
        applyMenuRequests();
        if (exit_requested_ || reviewing_exit_)
            return;
        if (!close_requests_.empty())
        {
            auto request = std::move(close_requests_.front());
            close_requests_.erase(close_requests_.begin());
            if (context->panes().find(request.pane.view()))
            {
                close_intent_ = std::move(request.pane);
                close_purpose_ = request.purpose;
            }
        }
        if (close_intent_.isValid())
            beginExitReview();
        if (reviewing_exit_)
            return;
        const auto count = asset_requests_.size();
        for (std::size_t index{}; index < count; ++index)
        {
            const auto id = asset_requests_[index];
            if (auto opened = context->openAsset(id); !opened)
                std::fprintf(
                    stderr,
                    "[editor.asset] %s: %s\n",
                    opened.error().domain.c_str(),
                    opened.error().message.c_str()
                );
        }
        asset_requests_.erase(asset_requests_.begin(), asset_requests_.begin() + count);
        rebuildMenu();
    }
    lux::cxx::expected<void, lux::ui::ECaptureError> Editor::drawDataReady(const lux::ui::DrawData& data) noexcept
    {
        return impl_->presentation->captureDrawData(data);
    }
    EditorContext& Editor::context() noexcept
    {
        return *impl_->context;
    }
    void Editor::event(object::EventView& event) noexcept
    {
        impl_->event(event);
    }
    void Editor::requestExit() noexcept
    {
        impl_->requestExit();
    }
    void Editor::fail(EditorFailure failure)
    {
        impl_->fail(std::move(failure));
    }
    int Editor::exec()
    {
        return impl_->exec();
    }
    bool Editor::closing() const noexcept
    {
        return impl_->exit_requested_;
    }
    const EditorResult<void>& Editor::outcome() const noexcept
    {
        return impl_->outcome_;
    }
}
