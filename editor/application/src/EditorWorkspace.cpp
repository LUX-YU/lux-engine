#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>
#include <random>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace lux::editor::application
{
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
        std::vector<ContentView> owners;
        std::vector<std::string> pane_names;
        EditorResult<void> result;
        auto create_input = [&](views::ViewTypeId type,
                                lux::ui::PaneId id) -> views::ViewFactoryResult<views::ViewFactoryInput> {
            if (content_views_.size() + owners.size() >= 64)
                return cxx::unexpected(views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "layout.capacity"}
                );
            const auto make_input = [&](auto value) {
                using Value = decltype(value);
                return views::ViewFactoryInput{
                    messages_.dispatcherRef(),
                    id,
                    contracts::CodeLease::builtin(),
                    cxx::typeToken<Value>(),
                    std::make_shared<const Value>(std::move(value))
                };
            };
            if (type == views::ViewTypeId{"lux.editor.scene.view"})
                return make_input(extensions::SceneViewInput{});
            if (type == views::ViewTypeId{"lux.editor.material"})
            {
                ContentView owner;
                owner.preview = std::make_unique<material::MaterialPreviewStore>(
                    engine_->sceneRuntime(),
                    material::MaterialPreviewEnvironment{environment_, registrations_.features}
                );
                auto input = make_input(MaterialViewAssembly{{}, owner.preview.get()});
                pane_names.emplace_back(id.name());
                owners.push_back(std::move(owner));
                return input;
            }
            if (type == views::ViewTypeId{"lux.editor.flowforge"})
                return make_input(FlowViewAssembly{});
            return make_input(std::monostate{}); // Exact factory argument validation rejects unsupported types.
        };
        auto apply = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void> {
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
            // Commit is already a fact. Every adopted preview now has its stable application owner.
            const auto all = desktop_->views().describeAll();
            if (!all)
                std::terminate();
            for (std::size_t i{}; i < owners.size(); ++i)
            {
                for (const auto& view : *all)
                {
                    bool matched{};
                    auto compare = [&](lux::ui::Pane& pane) { matched = pane.id().name() == pane_names[i]; };
                    auto visited = desktop_->views().withView(view.id, compare);
                    if (!visited)
                        std::terminate();
                    if (matched)
                    {
                        owners[i].view = view.id;
                        break;
                    }
                }
                if (!owners[i].view.valid())
                    std::terminate();
                content_views_.push_back(std::move(owners[i]));
            }
            return {};
        };
        auto entered = contributions_.withSnapshot(apply);
        if (!entered)
            return applicationFailure("layout.catalog", entered.error());
        return result;
    }
    EditorResult<void> EditorApplication::Impl::executeWorkspaceIntent(const WorkspaceIntent& intent)
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
        if (intent.action == EWorkspaceIntent::REFRESH)
            return refresh();
        if (intent.action == EWorkspaceIntent::ACKNOWLEDGE)
        {
            std::erase_if(workspace_publications_, [&](const auto& report) {
                return report.ticket == intent.ticket && report.result.has_value() &&
                       migration_ticket_ != report.ticket;
            });
            return {};
        }
        if (intent.action == EWorkspaceIntent::RECONCILE)
        {
            auto reconciled = writes_.reconcile(intent.ticket, files_);
            return reconciled ? EditorResult<void>{} : applicationFailure("workspace.reconcile", reconciled.error());
        }
        if (phase_ != EApplicationPhase::RUNNING || workspace_publications_.size() >= 16)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.admission"});
        if (intent.action == EWorkspaceIntent::CAPTURE_RECOVERY)
            return captureRecovery();
        if (intent.action == EWorkspaceIntent::RESTORE_RECOVERY)
            return restoreRecovery();
        if (intent.action == EWorkspaceIntent::MIGRATE)
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
        WorkspaceResult<WriteTicket> publication = cxx::unexpected(WorkspaceFailure{EWorkspaceError::INVALID_DATA});
        std::string label;
        switch (intent.action)
        {
        case EWorkspaceIntent::SAVE_LAYOUT: {
            std::mt19937 random{std::random_device{}()};
            workspace::LayoutId id{uuids::to_string(uuids::uuid_random_generator{random}())};
            std::erase(id.value, '-');
            auto layout = desktop_->views().captureLayout(id, intent.label);
            if (!layout)
                return applicationFailure("workspace.capture", layout.error());
            publication = workspace_.saveLayout(*layout, "missing");
            label = "Save layout: " + intent.label;
            break;
        }
        case EWorkspaceIntent::APPLY_LAYOUT: {
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
            break;
        }
        case EWorkspaceIntent::RENAME_LAYOUT:
            publication = workspace_.renameLayout(intent.layout, intent.label);
            label = "Rename layout: " + intent.label;
            break;
        case EWorkspaceIntent::REMOVE_LAYOUT:
            publication = workspace_.removeLayout(intent.layout);
            label = "Delete layout: " + intent.layout.value;
            break;
        default:
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "workspace.action"});
        }
        if (!publication)
            return applicationFailure("workspace.publication", publication.error());
        workspace_publications_.push_back({std::move(label), *publication});
        return {};
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
    void EditorApplication::Impl::installWorkspaceView(extensions::ContributionDraft& draft)
    {
        class WorkspacePane final : public lux::ui::Pane
        {
            struct Content final : lux::ui::Element
            {
                Impl& app_;
                std::string label_{"Workspace"};
                Content(WorkspacePane& parent, Impl& app) : Element(parent, lux::ui::ElementId{"workspace"}), app_(app)
                {
                    setStretch({1, 1});
                }
                void draw() noexcept override
                {
                    auto button = [&](const char* label, WorkspaceIntent intent) {
                        ImGui::BeginDisabled(app_.workspace_intent_.has_value());
                        if (ImGui::Button(label))
                            app_.workspace_intent_ = std::move(intent);
                        ImGui::EndDisabled();
                    };
                    ImGui::InputText("Layout label", &label_);
                    button("Save current layout as new", {EWorkspaceIntent::SAVE_LAYOUT, {}, label_});
                    ImGui::SameLine();
                    button("Refresh directory", {EWorkspaceIntent::REFRESH});
                    if (app_.workspace_failure_)
                        ImGui::TextWrapped(
                            "%s: %s",
                            app_.workspace_failure_->domain.c_str(),
                            app_.workspace_failure_->message.c_str()
                        );
                    for (const auto& diagnostic : app_.layout_catalog_.diagnostics)
                        ImGui::TextWrapped("%s: %s", diagnostic.file.c_str(), diagnostic.failure.detail.c_str());
                    for (const auto& layout : app_.layout_catalog_.layouts)
                    {
                        ImGui::PushID(layout.id.value.c_str());
                        ImGui::SeparatorText(layout.label.c_str());
                        button("Apply", {EWorkspaceIntent::APPLY_LAYOUT, layout.id});
                        ImGui::SameLine();
                        button("Rename to label", {EWorkspaceIntent::RENAME_LAYOUT, layout.id, label_});
                        ImGui::SameLine();
                        button("Delete", {EWorkspaceIntent::REMOVE_LAYOUT, layout.id});
                        ImGui::PopID();
                    }
                    ImGui::SeparatorText("Content recovery (independent of layouts)");
                    button("Record current locations", {EWorkspaceIntent::CAPTURE_RECOVERY});
                    button("Restore recorded content", {EWorkspaceIntent::RESTORE_RECOVERY});
                    button("Import old workspace data", {EWorkspaceIntent::MIGRATE});
                    if (app_.migration_)
                    {
                        for (const auto& diagnostic : app_.migration_->diagnostics)
                            ImGui::TextWrapped("%s", diagnostic.c_str());
                        if (app_.migration_complete_)
                            ImGui::TextUnformatted("Migration verified complete");
                        if (app_.migration_failure_)
                            ImGui::TextWrapped("%s", app_.migration_failure_->domain.c_str());
                    }
                    if (app_.recovery_)
                        for (const auto& item : app_.recovery_->items)
                        {
                            ImGui::TextWrapped(
                                "%s / %s",
                                std::string(item.entry.restore_key.name()).c_str(),
                                item.entry.locator.c_str()
                            );
                            if (item.entry.unpersisted_changes)
                                ImGui::TextUnformatted(
                                    "Only saved content can be restored; unsaved edits are not in this manifest."
                                );
                            if (item.failure)
                                ImGui::TextWrapped(
                                    "%s: %s",
                                    item.failure->domain.c_str(),
                                    item.failure->message.c_str()
                                );
                            else if (item.result)
                            {
                                if (item.result->presentation_failure)
                                    ImGui::TextWrapped(
                                        "Content retained, view unavailable: %s",
                                        item.result->presentation_failure->domain.c_str()
                                    );
                                else if (item.result->content.failure)
                                    ImGui::TextWrapped(
                                        "%s: %s",
                                        item.result->content.failure->domain.c_str(),
                                        item.result->content.failure->detail.c_str()
                                    );
                                else
                                    ImGui::TextUnformatted(item.result->view ? "Content presented" : "Not presented");
                            }
                            else
                                ImGui::TextUnformatted("Recovery pending");
                        }
                    ImGui::SeparatorText("Publication results");
                    for (const auto& report : app_.workspace_publications_)
                    {
                        ImGui::PushID(static_cast<int>(report.ticket.value));
                        ImGui::TextUnformatted(report.label.c_str());
                        if (report.result)
                        {
                            std::visit(
                                [](const auto& result) {
                                    if constexpr (std::same_as<
                                                      std::decay_t<decltype(result)>,
                                                      persistence::CommitReceipt>)
                                        ImGui::TextUnformatted("Published");
                                    else
                                        ImGui::TextWrapped("%s", result.failure.detail.c_str());
                                },
                                *report.result
                            );
                            if (report.catalog_failure)
                                ImGui::TextWrapped(
                                    "Directory refresh failed: %s",
                                    report.catalog_failure->detail.c_str()
                                );
                            button("Acknowledge", {EWorkspaceIntent::ACKNOWLEDGE, {}, {}, report.ticket});
                        }
                        else
                        {
                            auto current = app_.writes_.status(report.ticket);
                            if (current && current->stage == persistence::EWriteStage::UNKNOWN)
                            {
                                ImGui::TextUnformatted("Unknown publication; target remains reserved.");
                                button("Reconcile", {EWorkspaceIntent::RECONCILE, {}, {}, report.ticket});
                            }
                            else
                                ImGui::TextUnformatted("Publication pending");
                        }
                        ImGui::PopID();
                    }
                }
            } content_;
            bool initialized_{};
            void update() noexcept override
            {
                if (!initialized_ && !content_.app_.workspace_intent_)
                {
                    content_.app_.workspace_intent_ = WorkspaceIntent{EWorkspaceIntent::REFRESH};
                    initialized_ = true;
                }
            }

        public:
            WorkspacePane(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, Impl& app)
                : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.workspace"}, "Workspace"),
                  content_(*this, app)
            {
                setContent(content_);
            }
        };
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
                    std::make_unique<WorkspacePane>(input.dispatcher(), input.paneId(), *this)
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
