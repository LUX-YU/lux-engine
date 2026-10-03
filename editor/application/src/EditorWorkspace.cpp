#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_workspace{
        lux::editor::commands::CommandIdView{"lux.editor.workspace"},
        "Layouts and Recovery",
        "Window"
    };
}
namespace lux::editor::application
{
    using namespace lux::editor::project;
    EditorResult<void> EditorApplication::applyLayout(workspace::DockLayout layout)
    {
        if (auto ready = impl_->admission(); !ready)
            return ready;
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->applyLayout(std::move(layout));
    }
    EditorResult<void> EditorApplication::Impl::applyLayout(workspace::DockLayout layout)
    {
        if (phase_ != EApplicationPhase::RUNNING)
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "layout.application"});
        EditorResult<void> result;
        auto apply = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void> {
            result = workspace_actions_->apply(std::move(layout), snapshot.views());
            return {};
        };
        auto entered = contributions_.withSnapshot(apply);
        if (!entered)
            return applicationFailure("layout.catalog", entered.error());
        return result;
    }
    EditorResult<void> EditorApplication::Impl::executeWorkspaceIntent(const VWorkspaceIntent& request)
    {
        return std::visit([&](const auto& intent) -> EditorResult<void> {
            using Intent = std::decay_t<decltype(intent)>;
            if constexpr (std::same_as<Intent, RefreshWorkspace>)
                return workspace_changes_.refresh();
            else if constexpr (std::same_as<Intent, AcknowledgeWorkspace>)
                return workspace_changes_.acknowledge(intent.ticket);
            else if constexpr (std::same_as<Intent, ReconcileWorkspace>)
                return workspace_changes_.reconcile(intent.ticket);
            else
            {
                if (phase_ != EApplicationPhase::RUNNING)
                    return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "workspace.admission"});
                if constexpr (std::same_as<Intent, CaptureRecovery>)
                    return captureRecovery();
                else if constexpr (std::same_as<Intent, RestoreRecovery>)
                    return restoreRecovery();
                else if constexpr (std::same_as<Intent, MigrateWorkspace>)
                    return workspace_changes_.migrate();
                else if constexpr (std::same_as<Intent, SaveLayout>)
                    return workspace_actions_->save(intent.label);
                else if constexpr (std::same_as<Intent, RenameLayout>)
                    return workspace_changes_.rename(intent.layout, intent.label);
                else if constexpr (std::same_as<Intent, RemoveLayout>)
                    return workspace_changes_.remove(intent.layout);
                else if constexpr (std::same_as<Intent, ApplyLayout>)
                {
                    EditorResult<void> result;
                    auto apply = [&](const extensions::ContributionSnapshot& snapshot)
                        -> extensions::ContributionResult<void> {
                        result = workspace_actions_->apply(intent.layout, snapshot.views());
                        return {};
                    };
                    auto guarded = contributions_.withSnapshot(apply);
                    return guarded ? std::move(result) : applicationFailure("layout.catalog", guarded.error());
                }
                else
                    static_assert(sizeof(Intent) == 0, "Every workspace intent needs an explicit receiver");
            }
        }, request);
    }

    EditorResult<void> EditorApplication::Impl::settleWorkspace()
    {
        if (workspace_intent_)
        {
            auto intent = std::exchange(workspace_intent_, {});
            auto received = executeWorkspaceIntent(*intent);
            if (!received)
                workspace_failure_ = received.error();
            else
                workspace_failure_.reset();
        }
        auto updated = workspace_changes_.update(phase_ == EApplicationPhase::RUNNING);
        if (!updated)
            return updated;
        return settleRecovery();
    }
    EditorResult<project::WorkspaceSnapshot> EditorApplication::Impl::observeWorkspace()
    {
        project::WorkspaceSnapshot snapshot;
        snapshot.catalog = workspace_changes_.catalog();
        if (workspace_failure_)
            snapshot.diagnostics.push_back(workspace_failure_->domain + ": " + workspace_failure_->message);
        if (const auto* migration = workspace_changes_.migration())
        {
            snapshot.diagnostics.insert(snapshot.diagnostics.end(), migration->diagnostics().begin(),
                migration->diagnostics().end());
            if (workspace_changes_.migrationComplete())
                snapshot.diagnostics.emplace_back("Migration verified complete");
            if (const auto* error = workspace_changes_.migrationFailure())
                snapshot.diagnostics.push_back(error->domain + ": " + error->message);
        }
        if (recovery_)
            for (const auto& item : recovery_->items)
            {
                snapshot.recovery.emplace_back(item.entry.restore_key.name());
                for (const auto& content : item.entry.contents)
                {
                    snapshot.recovery.push_back(content.locator);
                    if (content.unpersisted_changes)
                        snapshot.recovery.emplace_back("Only saved content can be restored; unsaved edits are not in this manifest.");
                }
                if (!item.result)
                    snapshot.recovery.emplace_back("Recovery pending");
                else if (!*item.result)
                    snapshot.recovery.push_back(item.result->error().domain + ": " + item.result->error().message);
                else
                    snapshot.recovery.emplace_back("Content presented");
                for (const auto& source : item.sources)
                    if (source.failure)
                        snapshot.recovery.push_back(source.failure->domain + ": " + source.failure->detail);
            }
        for (const auto& report : workspace_changes_.publications())
        {
            project::WorkspacePublicationInfo row{report.label, report.ticket, report.result};
            if (report.catalog_failure)
                row.catalog_failure = report.catalog_failure->detail;
            if (!report.result)
            {
                auto status = writes_.status(report.ticket);
                if (!status)
                    return applicationFailure("workspace.observation", status.error());
                row.unknown = status->stage == persistence::EWriteStage::UNKNOWN;
            }
            snapshot.publications.push_back(std::move(row));
        }
        return snapshot;
    }

    void EditorApplication::Impl::installWorkspaceView(extensions::ContributionDraft& draft)
    {
        // The same recovery operations are available to menus, scripts and installed workbench consumers.
        // Commands retain the existing workspace owner, admission and publication path.
        const auto recovery_command = [&](const char* id, const char* label, VWorkspaceIntent intent) {
            draft.commands.push_back(commands::CommandEntry::create(
                contracts::CodeLease::builtin(),
                commands::CommandDescriptor{commands::CommandIdView{id}, label, "Workspace"},
                [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                    return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
                },
                [this, intent = std::move(intent)](const commands::CommandInvocation&)
                    -> commands::CommandResult<commands::DispatchReceipt> {
                    auto requested = executeWorkspaceIntent(intent);
                    if (!requested)
                        return cxx::unexpected(commands::CommandFailure{
                            commands::ECommandError::DOMAIN_FAILURE,
                            requested.error().domain,
                            requested.error().reason,
                            requested.error().message
                        });
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            ));
        };
        recovery_command("lux.editor.recovery.capture", "Record content locations", CaptureRecovery{});
        recovery_command("lux.editor.recovery.restore", "Restore recorded content", RestoreRecovery{});
        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.workspace"},
                "Workspace",
                cxx::typeToken<std::monostate>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                return views::DetachedView{
                    contracts::CodeLease::builtin(),
                    std::make_unique<project::WorkspaceView>(input.dispatcher(), input.paneId(),
                        [this] { return observeWorkspace(); },
                        [this](VWorkspaceIntent intent) -> EditorResult<void> {
                            if (workspace_intent_)
                                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.intent.capacity"});
                            workspace_intent_ = std::move(intent);
                            return {};
                        }
                    )
                };
            }
        ));
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_workspace>(
            contracts::CodeLease::builtin(),
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto shown = showTool(views::ViewTypeId{"lux.editor.workspace"});
                if (!shown)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        shown.error().domain,
                        shown.error().reason,
                        shown.error().message
                    });
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
    }

}
