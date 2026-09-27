#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <lux/engine/editor/detail/TaskResult.hpp>
#include <algorithm>

namespace lux::editor
{
    void Editor::Impl::startWorkspace()
    {
        workspace_pending_ = true;
        const auto blocking = engine->execution().blocking();
        if (!blocking)
        {
            workspace_result_.emplace(
                lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "workspace.scheduler"})
            );
            return;
        }
        auto admitted = engine->execution().submit(
            {"Read editor workspace", "Editor settings"},
            [scheduler = *blocking, project = project_->root()](process::TaskReporter) noexcept {
                return stdexec::then(stdexec::schedule(scheduler), [project]() {
                    return detail::readWorkspace(project, {});
                });
            },
            [this](process::TTaskResult<detail::WorkspaceData, EditorFailure>&& result) noexcept {
                workspace_result_.emplace(detail::taskResult(std::move(result)));
            }
        );
        if (admitted)
            workspace_task_ = std::move(*admitted);
        else
        {
            workspace_result_.emplace(lux::cxx::unexpected(
                EditorFailure{EEditorError::EXECUTION_FAILURE, "workspace.submit", 0, {}, admitted.error()}
            ));
        }
    }
    EditorResult<detail::WorkspaceData> Editor::Impl::captureWorkspace()
    {
        detail::WorkspaceData snapshot;
        snapshot.selected = workspace_.selected;
        const auto entries = context->panes().registrations();
        for (const auto& pane : context->panes().panes())
        {
            const auto entry = std::ranges::find(entries, pane->type(), &PaneRegistration::type);
            if (entry == entries.end() || !entry->capture || !entry->restore)
                continue;
            FinishEditingRequest finished;
            static_cast<void>(object::sendEvent(*pane, finished));
            if (!finished.result)
                return lux::cxx::unexpected(finished.result.error());
            auto payload = entry->capture(context->panes(), *pane);
            if (!payload)
                return lux::cxx::unexpected(payload.error());
            snapshot.panes.push_back({pane->type(), pane->id(), pane->visible(), std::move(*payload)});
        }
        // Missing plugins keep their opaque state, including docking entries retained by ImGui.
        for (const auto& old : unrestored_panes_)
        {
            const bool present =
                std::ranges::any_of(snapshot.panes, [&](const auto& pane) { return pane.id == old.id; });
            if (!present)
                snapshot.panes.push_back(old);
        }
        for (const auto* pane : root->panes())
            if (pane && pane->parent() != root && !pane->modal())
                snapshot.child_visibility.emplace_back(pane->id(), pane->visible());
        for (const auto& old : workspace_.child_visibility)
            if (!root->findPane(old.first.view()))
                snapshot.child_visibility.push_back(old);
        snapshot.dock = root->captureDockState();
        return snapshot;
    }
    EditorResult<void> Editor::Impl::restoreWorkspace(const detail::WorkspaceData& data)
    {
        if (data.dock.bytes().empty())
            return {};
        // End interactions before touching any window placement or adopting a missing window.
        for (const auto& pane : context->panes().panes())
        {
            FinishEditingRequest finished;
            static_cast<void>(object::sendEvent(*pane, finished));
            if (!finished.result)
                return finished.result;
        }
        std::vector<lux::ui::DockIdentity> identities;
        std::vector<PaneState> unrestored;
        const auto entries = context->panes().registrations();
        for (const auto& saved : data.panes)
        {
            const auto entry = std::ranges::find(entries, saved.type, &PaneRegistration::type);
            if (entry == entries.end() || !entry->restore)
            {
                unrestored.push_back(saved);
                reportMenuFailure({EEditorError::MISSING_PROVIDER, "workspace.pane", 0, std::string(saved.type.name())}
                );
                continue;
            }
            // Keep the factory and its code owner alive through construction and error destruction.
            const auto registration = *entry;
            const auto restored = registration.restore(context->panes(), saved);
            if (!restored)
            {
                unrestored.push_back(saved);
                reportMenuFailure(restored.error());
                continue;
            }
            auto& pane = restored->get();
            identities.push_back({std::string(saved.id.name()), std::string(pane.id().name())});
            pane.setVisible(saved.visible);
        }
        const auto restored = root->restoreDockState(data.dock, identities);
        if (!restored)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "workspace.docking", 0, {}, restored.error()}
            );
        for (const auto& [saved_id, visible] : data.child_visibility)
        {
            for (const auto& identity : identities)
            {
                const auto id = saved_id.name();
                const bool belongs = id.starts_with(identity.saved) && id.size() > identity.saved.size() &&
                                     id[identity.saved.size()] == '/';
                if (!belongs)
                    continue;
                const auto current = identity.current + std::string(id.substr(identity.saved.size()));
                auto* pane = root->findPane(lux::ui::PaneIdView{current});
                if (pane && pane->parent() != root && !pane->modal())
                    pane->setVisible(visible);
                break;
            }
        }
        unrestored_panes_ = std::move(unrestored);
        return {};
    }
    EditorResult<void> Editor::Impl::defaultWorkspace()
    {
        for (const auto& pane : context->panes().panes())
        {
            FinishEditingRequest finished;
            static_cast<void>(object::sendEvent(*pane, finished));
            if (!finished.result)
            {
                return finished.result;
            }
        }
        root->resetDockLayout();
        return {};
    }
    void Editor::Impl::workspaceRequest(WorkspaceRequest& request)
    {
        if (request.action == EWorkspaceAction::STATUS)
        {
            if (request.revision != workspace_revision_)
            {
                request.layouts = workspace_.names;
                request.message = workspace_message_;
            }
            request.revision = workspace_revision_;
            request.pending = workspace_pending_ || bool(workspace_intent_);
            return;
        }
        if (workspace_pending_ || reviewing_exit_)
        {
            request.result = lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace"});
            return;
        }
        const bool invalid_name = request.action != EWorkspaceAction::DEFAULT && !detail::validLayoutName(request.name);
        const bool invalid_new_name =
            request.action == EWorkspaceAction::RENAME && !detail::validLayoutName(request.new_name);
        if (invalid_name || invalid_new_name)
        {
            request.result = lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "workspace.name"});
            return;
        }
        detail::WorkspaceData captured = workspace_;
        if (request.action == EWorkspaceAction::SAVE)
        {
            auto snapshot = captureWorkspace();
            if (!snapshot)
            {
                request.result = lux::cxx::unexpected(snapshot.error());
                return;
            }
            captured = std::move(*snapshot);
        }
        if (request.action == EWorkspaceAction::DEFAULT)
        {
            request.result = defaultWorkspace();
            if (!request.result)
                return;
        }
        const auto scheduler = engine->execution().blocking();
        if (!scheduler)
        {
            request.result =
                lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "workspace.scheduler"});
            return;
        }
        const auto action = request.action;
        auto admitted = engine->execution().submit(
            {"Update editor workspace", "Editor settings"},
            [scheduler = *scheduler,
             project = project_->root(),
             action,
             name = request.name,
             new_name = request.new_name,
             data = std::move(captured)](process::TaskReporter) mutable noexcept {
                return stdexec::then(
                    stdexec::schedule(scheduler),
                    [project, action, name = std::move(name), new_name = std::move(new_name), data = std::move(data)](
                    ) mutable {
                        if (action == EWorkspaceAction::APPLY)
                            return detail::readWorkspace(project, std::move(name));
                        return detail::writeWorkspace(
                            project,
                            action,
                            std::move(name),
                            std::move(new_name),
                            std::move(data)
                        );
                    }
                );
            },
            [this](process::TTaskResult<detail::WorkspaceData, EditorFailure>&& result) noexcept {
                workspace_result_.emplace(detail::taskResult(std::move(result)));
            }
        );
        if (!admitted)
        {
            request.result = lux::cxx::unexpected(
                EditorFailure{EEditorError::EXECUTION_FAILURE, "workspace.submit", 0, {}, admitted.error()}
            );
            return;
        }
        workspace_task_ = std::move(*admitted);
        workspace_action_ = action;
        workspace_pending_ = true;
        workspace_message_ = "Working...";
        request.pending = true;
    }
    void Editor::Impl::applyWorkspaceResult()
    {
        if (!workspace_result_ || reviewing_exit_)
            return;
        auto& result = *workspace_result_;
        if (result && workspace_action_ == EWorkspaceAction::APPLY)
        {
            const auto restored = restoreWorkspace(*result);
            if (!restored && restored.error().code == EEditorError::BUSY)
                return;
            if (!restored)
                result = lux::cxx::unexpected(restored.error());
            else if (!workspace_startup_)
            {
                // The restored state is already visible. Persist its selection without restoring it a second time.
                workspace_ = *result;
                workspace_result_.reset();
                workspace_task_ = {};
                const auto scheduler = engine->execution().blocking();
                auto admitted = engine->execution().submit(
                    {"Remember active layout", "Editor settings"},
                    [scheduler = *scheduler, project = project_->root(), data = workspace_](process::TaskReporter
                    ) noexcept {
                        return stdexec::then(stdexec::schedule(scheduler), [project, data]() mutable {
                            const auto name = data.selected;
                            return detail::writeWorkspace(project, EWorkspaceAction::APPLY, name, {}, std::move(data));
                        });
                    },
                    [this](process::TTaskResult<detail::WorkspaceData, EditorFailure>&& completed) noexcept {
                        workspace_result_.emplace(detail::taskResult(std::move(completed)));
                    }
                );
                workspace_action_ = EWorkspaceAction::STATUS;
                if (admitted)
                    workspace_task_ = std::move(*admitted);
                else
                    workspace_result_.emplace(lux::cxx::unexpected(
                        EditorFailure{EEditorError::EXECUTION_FAILURE, "workspace.selection", 0, {}, admitted.error()}
                    ));
                ++workspace_revision_;
                menu_dirty_ = true;
                return;
            }
        }
        if (result)
        {
            workspace_ = std::move(*result);
            workspace_message_ = "Workspace updated";
        }
        else
        {
            workspace_message_ = result.error().domain + ": " + result.error().message;
            reportMenuFailure(result.error());
        }
        workspace_result_.reset();
        workspace_task_ = {};
        workspace_pending_ = false;
        ++workspace_revision_;
        menu_dirty_ = true;
        if (std::exchange(workspace_startup_, false) && workspace_.selected.empty())
        {
            lux::ui::Command command{lux::ui::CommandIdView{"lux.product.default"}, lux::ui::ECommandPhase::EXECUTE};
            applicationCommand(command);
        }
    }
}
