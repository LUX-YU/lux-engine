#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>
#include <random>

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
            auto create_input = [&](views::ViewTypeId type, lux::ui::PaneId id)
                -> views::ViewFactoryResult<views::ViewFactoryInput> {
                const auto entries = snapshot.views().entries();
                const auto factory = std::ranges::find_if(entries, [&](const auto& entry) {
                    return entry->descriptor().type == type;
                });
                if (factory == entries.end())
                    return cxx::unexpected(views::ViewFactoryFailure{views::EViewFactoryError::NOT_FOUND, "layout.view"});
                const auto make_input = [&](auto value) {
                    using Value = decltype(value);
                    return views::ViewFactoryInput{
                        messages_.dispatcherRef(), id, contracts::CodeLease::builtin(),
                        cxx::typeToken<Value>(), std::make_shared<const Value>(std::move(value))
                    };
                };
                if ((*factory)->descriptor().binding_type == cxx::typeToken<views::ContentViewInput>())
                    return make_input(views::ContentViewInput{});
                return make_input(std::monostate{});
            };
            auto prepared = desktop_->views().prepareLayout(std::move(layout), snapshot.views(), create_input);
            if (!prepared)
            {
                result = applicationFailure("layout.prepare", prepared.error());
                return {};
            }
            auto committed = desktop_->views().commit(*prepared);
            if (!committed)
            {
                result = applicationFailure("layout.commit", committed.error());
                return {};
            }
            return {};
        };
        auto entered = contributions_.withSnapshot(apply);
        if (!entered)
            return applicationFailure("layout.catalog", entered.error());
        return result;
    }
    EditorResult<void> EditorApplication::Impl::executeWorkspaceIntent(const VWorkspaceIntent& request)
    {
        using namespace workspace;
        using namespace persistence;
        const auto refresh = [&]() -> EditorResult<void> {
            auto catalog = workspace_.listLayouts();
            if (!catalog)
                return applicationFailure("workspace.catalog", catalog.error());
            layout_catalog_ = std::move(*catalog); // Partial catalogs retain their per-file diagnostics.
            return {};
        };
        return std::visit([&](const auto& intent) -> EditorResult<void> {
            using Intent = std::decay_t<decltype(intent)>;
            if constexpr (std::same_as<Intent, RefreshWorkspace>)
                return refresh();
            else if constexpr (std::same_as<Intent, AcknowledgeWorkspace>)
            {
                std::erase_if(workspace_publications_, [&](const auto& report) {
                    return report.ticket == intent.ticket && report.result.has_value() &&
                           migration_ticket_ != report.ticket;
                });
                return {};
            }
            else if constexpr (std::same_as<Intent, ReconcileWorkspace>)
            {
                auto reconciled = writes_.reconcile(intent.ticket, files_);
                return reconciled ? EditorResult<void>{} : applicationFailure("workspace.reconcile", reconciled.error());
            }
            else
            {
                const bool is_stopping = phase_ != EApplicationPhase::RUNNING;
                const bool is_full = workspace_publications_.size() >= 16;
                if (is_stopping || is_full)
                    return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.admission"});
                if constexpr (std::same_as<Intent, CaptureRecovery>)
                    return captureRecovery();
                else if constexpr (std::same_as<Intent, RestoreRecovery>)
                    return restoreRecovery();
                else if constexpr (std::same_as<Intent, MigrateWorkspace>)
                {
                    if (migration_ticket_)
                        return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.migration.pending"});
                    auto input = workspace_.prepareLegacyMigration();
                    if (!input)
                        return applicationFailure("workspace.migration.read", input.error());
                    migration_ = std::move(*input);
                    migration_failure_.reset();
                    migration_complete_ = false;
                    return {};
                }
                else
                {
                    WorkspaceResult<WriteTicket> publication = cxx::unexpected(WorkspaceFailure{EWorkspaceError::INVALID_DATA});
                    std::string label;
                    if constexpr (std::same_as<Intent, SaveLayout>)
                    {
                        std::mt19937 random{std::random_device{}()};
                        workspace::LayoutId id{uuids::to_string(uuids::uuid_random_generator{random}())};
                        std::erase(id.value, '-');
                        auto layout = desktop_->views().captureLayout(id, intent.label);
                        if (!layout)
                            return applicationFailure("workspace.capture", layout.error());
                        publication = workspace_.saveLayout(*layout, "missing");
                        label = "Save layout: " + intent.label;
                    }
                    else if constexpr (std::same_as<Intent, ApplyLayout>)
                    {
                        auto layout = workspace_.readLayout(intent.layout);
                        if (!layout)
                            return applicationFailure("workspace.read", layout.error());
                        auto applied = applyLayout(std::move(layout->value));
                        if (!applied)
                            return applied;
                        // UI commit is a fact. Preferences are a separate write, with their own retained result.
                        auto previous = workspace_.readPreferences();
                        if (!previous && previous.error().code != EWorkspaceError::NOT_FOUND)
                            return applicationFailure("workspace.applied.preferences-read", previous.error());
                        auto preferences = previous ? std::move(previous->value) : UserPreferences{};
                        const auto version = previous ? previous->target.expected_version : "missing";
                        preferences.selected_layout = intent.layout;
                        publication = workspace_.writePreferences(preferences, version);
                        label = "Applied layout; persist selection";
                    }
                    else if constexpr (std::same_as<Intent, RenameLayout>)
                    {
                        publication = workspace_.renameLayout(intent.layout, intent.label);
                        label = "Rename layout: " + intent.label;
                    }
                    else if constexpr (std::same_as<Intent, RemoveLayout>)
                    {
                        publication = workspace_.removeLayout(intent.layout);
                        label = "Delete layout: " + intent.layout.value;
                    }
                    else
                        static_assert(sizeof(Intent) == 0, "Every workspace action requires an explicit receiver");
                    if (!publication)
                        return applicationFailure("workspace.publication", publication.error());
                    workspace_publications_.push_back({std::move(label), *publication});
                    return {};
                }
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
        for (auto& report : workspace_publications_)
        {
            if (report.result)
                continue;
            auto status = writes_.status(report.ticket);
            if (!status)
                return applicationFailure("workspace.status", status.error());
            if (status->stage != persistence::EWriteStage::TERMINAL)
                continue;
            report.result = status->outcome;
            auto acknowledged = writes_.acknowledge(report.ticket);
            if (!acknowledged)
                return applicationFailure("workspace.acknowledge", acknowledged.error());
            auto catalog = workspace_.listLayouts();
            if (catalog)
                layout_catalog_ = std::move(*catalog);
            else
                report.catalog_failure = catalog.error(); // Never replace the last catalog with an empty one.
        }
        auto migrated = settleMigration();
        if (!migrated)
            return migrated;
        return settleRecovery();
    }
    EditorResult<project::WorkspaceSnapshot> EditorApplication::Impl::observeWorkspace()
    {
        project::WorkspaceSnapshot snapshot;
        snapshot.catalog = layout_catalog_;
        if (workspace_failure_)
            snapshot.diagnostics.push_back(workspace_failure_->domain + ": " + workspace_failure_->message);
        if (migration_)
        {
            snapshot.diagnostics.insert(snapshot.diagnostics.end(), migration_->diagnostics.begin(),
                migration_->diagnostics.end());
            if (migration_complete_)
                snapshot.diagnostics.emplace_back("Migration verified complete");
            if (migration_failure_)
                snapshot.diagnostics.push_back(migration_failure_->domain + ": " + migration_failure_->message);
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
        for (const auto& report : workspace_publications_)
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
            draft.commands.push_back(std::make_shared<commands::CommandEntry>(
                contracts::CodeLease::builtin(),
                commands::CommandDescriptor{commands::CommandId{id}, label, "Workspace"},
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
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{commands::CommandId{"lux.editor.workspace"}, "Layouts and Recovery", "Window"},
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
