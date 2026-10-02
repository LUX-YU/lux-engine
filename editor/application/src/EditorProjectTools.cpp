#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/window/FileDialog.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/engine/ui/Element.hpp>
#include <toml++/toml.hpp>
#include <imgui.h>
#include <sstream>

namespace lux::editor::application
{
    void EditorApplication::Impl::installProjectTools(extensions::ContributionDraft& draft)
    {
        installRecentProjects(draft);
        installSettingsView(draft);
        installProjectCreation(draft);
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{commands::CommandId{"lux.editor.initial-scene"}, "Open Initial Scene", "File"},
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{
                    phase_ == EApplicationPhase::RUNNING && !project_->manifest().default_scene.empty()
                };
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                const auto& manifest = project_->manifest();
                const auto found =
                    std::ranges::find(manifest.assets, manifest.default_scene, &ProjectAssetEntry::source_path);
                if (found == manifest.assets.end())
                    return cxx::unexpected(
                        commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "initial-scene.missing"}
                    );
                if (open_intents_.size() == 64)
                    return cxx::unexpected(
                        commands::CommandFailure{commands::ECommandError::CAPACITY, "initial-scene.queue"}
                    );
                // The command dispatch retains its protection; factory admission belongs to the next owner batch.
                open_intents_.push_back(project_->catalogModel().reference(found->id));
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));

        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{
                commands::CommandId{"lux.editor.project.open"},
                "Open Project in New Editor",
                "File"
            },
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{
                    phase_ == EApplicationPhase::RUNNING && !project_launch_ && !project_open_requested_ &&
                    !project_launch_intent_
                };
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                if (project_launch_ || project_open_requested_ || project_launch_intent_)
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "project.open"});
                project_open_requested_ = true;
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));

        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.import"},
                "Import Assets",
                cxx::typeToken<std::monostate>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                std::erase_if(connections_, [](const auto& value) { return !value.connected(); });
                if (connections_.size() >= 64)
                    return cxx::unexpected(
                        views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "import.connections"}
                    );
                auto view = std::make_unique<project::ImportView>(
                    input.dispatcher(),
                    input.paneId(),
                    project_->catalogModel(),
                    *importer_
                );
                auto connected = object::LuxObject::connect(
                    view.get(),
                    &project::ImportView::browseRequested,
                    [this, pane = input.paneId()]() noexcept { import_browse_ = pane; }
                );
                if (!connected)
                    return cxx::unexpected(
                        views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "import.browse"}
                    );
                connections_.push_back(std::move(*connected));
                return views::DetachedView{contracts::CodeLease::builtin(), std::move(view)};
            }
        ));
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{commands::CommandId{"lux.editor.import"}, "Import Assets", "File"},
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto shown = showTool(views::ViewTypeId{"lux.editor.import"});
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
    EditorResult<void> EditorApplication::Impl::receiveProjectIntents()
    {
        if (std::exchange(project_open_requested_, false))
        {
            const std::array filters{window::FileDialogFilter{"Lux project", "luxproject"}};
            auto selected = window::openFileDialog(window_.get(), filters);
            if (!selected)
                return applicationFailure("project.dialog", selected.error());
            if (*selected)
                project_launch_intent_ = std::move(**selected);
        }
        if (project_launch_intent_ && !project_launch_)
        {
            auto accepted = project_tasks_.submit(
                {"Open project in Editor", "Project"},
                [file = *project_launch_intent_,
                 installation = config_.installation,
                 scheduler = *engine_->execution().blocking()](process::TaskReporter) noexcept {
                    return stdexec::then(stdexec::schedule(scheduler), [file, installation]() noexcept {
                        return launchEditor(installation, file);
                    });
                },
                [this](process::TTaskResult<void, EditorFailure>&& result) noexcept {
                    project_launch_.reset();
                    if (result)
                        project_launch_result_.emplace();
                    else if (auto* error = result.error().domainFailure())
                        project_launch_result_.emplace(cxx::unexpected(std::move(*error)));
                    else
                        project_launch_result_.emplace(applicationFailure("project.launch.task", result.error()));
                }
            );
            if (!accepted)
                return applicationFailure("project.launch.submit", accepted.error());
            project_launch_ = *accepted;
            project_launch_intent_.reset();
        }
        if (!import_browse_)
            return {};
        const auto target = std::exchange(import_browse_, {});
        auto all = desktop_->views().describeAll();
        if (!all)
            return applicationFailure("import.browse.views", all.error());
        std::optional<views::ViewId> found;
        for (const auto& view : *all)
        {
            auto compare = [&](lux::ui::Pane& pane) {
                if (pane.id() == *target && pane.type() == lux::ui::PaneTypeId{"lux.editor.import"})
                    found = view.id;
            };
            auto visited = desktop_->views().withView(view.id, compare);
            if (!visited)
                return applicationFailure("import.browse.target", visited.error());
        }
        if (!found)
            return {}; // A closed UI cannot redirect its native result to a later window.
        auto chosen = window::openFileDialog(window_.get());
        auto deliver = [&](lux::ui::Pane& pane) {
            auto& view = static_cast<project::ImportView&>(pane);
            if (!chosen)
                view.showFailure(EditorFailure{EEditorError::SOURCE_FAILURE, "import.browse", 0, chosen.error().detail}
                );
            else if (*chosen)
                view.setSource(std::move(**chosen));
        };
        auto delivered = desktop_->views().withView(*found, deliver);
        if (!delivered)
            return applicationFailure("import.browse.deliver", delivered.error());
        return {};
    }
}
namespace lux::editor::application
{
    void EditorApplication::Impl::maintainRecentProjects()
    {
        if (recent_ticket_)
        {
            if (std::exchange(recent_reconcile_, false))
                if (auto result = writes_.reconcile(*recent_ticket_, files_); !result)
                    recent_failure_ = applicationFailure("recent.reconcile", result.error()).value();
            const auto status = writes_.status(*recent_ticket_);
            if (status && status->stage == persistence::EWriteStage::TERMINAL)
            {
                recent_publication_ = status->outcome;
                if (auto acknowledged = writes_.acknowledge(*recent_ticket_); acknowledged)
                    recent_ticket_.reset();
                else
                    recent_failure_ = applicationFailure("recent.acknowledge", acknowledged.error()).value();
            }
        }
        if (recent_result_)
        {
            if (!*recent_result_)
            {
                recent_failure_ = recent_result_->error();
                recent_result_.reset();
            }
            else if (phase_ != EApplicationPhase::RUNNING)
                recent_result_.reset(); // Reading is complete; no new write is admitted while closing.
            else
            {
                auto& value = **recent_result_;
                auto ticket = persistence::publishEncodedArtifact(writes_, value.target, value.encoded);
                if (ticket)
                {
                    recent_projects_ = std::move(value.paths);
                    recent_ticket_ = *ticket;
                    recent_publication_.reset();
                    recent_failure_.reset();
                    recent_result_.reset();
                }
                else if (ticket.error().code != persistence::EPersistenceError::BUSY &&
                         ticket.error().code != persistence::EPersistenceError::CAPACITY)
                {
                    recent_failure_ = applicationFailure("recent.publish", ticket.error()).value();
                    recent_result_.reset();
                }
            }
        }
        if (!recent_requested_ || recent_task_ || recent_result_ || recent_ticket_ ||
            phase_ != EApplicationPhase::RUNNING)
            return;
        const auto path = *config_.user_directory / "lux/editor/recent-projects.toml";
        auto accepted = project_tasks_.submit(
            {"Read recent projects", "Preferences"},
            [path,
             directory = *config_.user_directory,
             project = config_.project_file,
             scheduler = *engine_->execution().blocking()](process::TaskReporter) noexcept {
                return stdexec::then(
                    stdexec::schedule(scheduler),
                    [path, directory, project]() -> EditorResult<RecentProjects> {
                        const auto utf8 = [](const std::filesystem::path& value) {
                            const auto bytes = value.generic_u8string();
                            return std::string{bytes.begin(), bytes.end()};
                        };
                        storage::FileArtifactStore store(directory);
                        auto target = store.resolve(utf8(path));
                        if (!target)
                            return applicationFailure("recent.read", target.error());
                        std::vector<std::filesystem::path> paths{project};
                        if (target->expected_version != "missing")
                        {
                            auto bytes = storage::readPublicationFile(path, 64 * 1024);
                            if (!bytes)
                                return applicationFailure("recent.read", bytes.error());
                            if (storage::publicationDigest(*bytes) != target->expected_version)
                                return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.changed"});
                            const std::string_view text{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
                            auto parsed = toml::parse(text);
                            if (!parsed || parsed["version"].value_or(0) != 1 || !parsed["projects"].is_array())
                                return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.format"});
                            for (const auto& row : *parsed["projects"].as_array())
                            {
                                auto value = row.value<std::string>();
                                if (!value || value->empty() || value->size() > 4096)
                                    return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.entry"});
                                auto candidate = std::filesystem::u8path(*value).lexically_normal();
                                if (!candidate.is_absolute())
                                    return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.path"});
                                if (paths.size() < 20 && std::ranges::find(paths, candidate) == paths.end())
                                    paths.push_back(std::move(candidate));
                            }
                        }
                        toml::array rows;
                        for (const auto& value : paths)
                            rows.push_back(utf8(value));
                        std::ostringstream output;
                        output << toml::table{{"version", 1}, {"projects", std::move(rows)}};
                        const auto encoded = output.str();
                        if (encoded.size() > 64 * 1024)
                            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "recent.bytes"});
                        const auto bytes = std::as_bytes(std::span{encoded.data(), encoded.size()});
                        return RecentProjects{
                            std::move(paths),
                            std::move(*target),
                            persistence::EncodedArtifact{{bytes.begin(), bytes.end()}}
                        };
                    }
                );
            },
            [this](process::TTaskResult<RecentProjects, EditorFailure>&& result) noexcept {
                recent_task_.reset();
                if (result)
                    recent_result_.emplace(std::move(*result));
                else if (auto* error = result.error().domainFailure())
                    recent_result_.emplace(cxx::unexpected(std::move(*error)));
                else
                    recent_result_.emplace(applicationFailure("recent.task", result.error()));
            }
        );
        if (accepted)
        {
            recent_task_ = *accepted;
            recent_requested_ = false;
        }
        else
            recent_failure_ = applicationFailure("recent.submit", accepted.error()).value();
    }
    void EditorApplication::Impl::installRecentProjects(extensions::ContributionDraft& draft)
    {
        class RecentPane final : public lux::ui::Pane
        {
            struct Content final : lux::ui::Element
            {
                Impl& app_;
                Content(RecentPane& pane, Impl& app) : Element(pane, lux::ui::ElementId{"recent"}), app_(app)
                {
                    setStretch({1, 1});
                }
                void draw() noexcept override
                {
                    if (app_.recent_failure_)
                        ImGui::TextWrapped(
                            "%s: %s",
                            app_.recent_failure_->domain.c_str(),
                            app_.recent_failure_->message.c_str()
                        );
                    if (app_.recent_publication_)
                        std::visit(
                            [](const auto& value) {
                                if constexpr (!std::same_as<std::decay_t<decltype(value)>, persistence::CommitReceipt>)
                                    ImGui::TextWrapped("Preferences publication: %s", value.failure.detail.c_str());
                            },
                            *app_.recent_publication_
                        );
                    if (ImGui::Button("Refresh recent projects"))
                        app_.recent_requested_ = true;
                    if (app_.recent_ticket_)
                    {
                        auto status = app_.writes_.status(*app_.recent_ticket_);
                        if (status && status->stage == persistence::EWriteStage::UNKNOWN)
                            if (ImGui::Button("Reconcile publication"))
                                app_.recent_reconcile_ = true;
                    }
                    ImGui::BeginDisabled(
                        app_.project_launch_.has_value() || app_.project_launch_intent_.has_value() ||
                        app_.phase_ != EApplicationPhase::RUNNING
                    );
                    for (const auto& value : app_.recent_projects_)
                    {
                        const auto text = value.generic_u8string();
                        if (ImGui::Button(reinterpret_cast<const char*>(text.c_str())))
                            app_.project_launch_intent_ = value; // Owned path survives subsequent refresh.
                    }
                    ImGui::EndDisabled();
                }
            } content_;

        public:
            RecentPane(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, Impl& app)
                : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.recent-projects"}, "Recent Projects"),
                  content_(*this, app)
            {
                setContent(content_);
            }
        };
        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.recent-projects"},
                "Recent Projects",
                cxx::typeToken<std::monostate>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                return views::DetachedView{
                    contracts::CodeLease::builtin(),
                    std::make_unique<RecentPane>(input.dispatcher(), input.paneId(), *this)
                };
            }
        ));
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{commands::CommandId{"lux.editor.project.recent"}, "Recent Projects", "File"},
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto shown = showTool(views::ViewTypeId{"lux.editor.recent-projects"});
                if (!shown)
                    return cxx::unexpected(
                        commands::CommandFailure{commands::ECommandError::DOMAIN_FAILURE, shown.error().domain}
                    );
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{
                commands::CommandId{"lux.editor.about"},
                "Lux Editor " LUX_EDITOR_VERSION,
                "Help"
            },
            [](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{false};
            },
            [](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                return cxx::unexpected(commands::CommandFailure{commands::ECommandError::DISABLED});
            }
        ));
    }
}
