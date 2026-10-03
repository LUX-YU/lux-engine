#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/editor/project/RecentProjectsView.hpp>
#include <lux/engine/window/FileDialog.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_initial_scene{
        lux::editor::commands::CommandIdView{"lux.editor.initial-scene"},
        "Open Initial Scene",
        "File"
    };
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_project_open{
        lux::editor::commands::CommandIdView{"lux.editor.project.open"},
        "Open Project in New Editor",
        "File"
    };
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_import{
        lux::editor::commands::CommandIdView{"lux.editor.import"},
        "Import Assets",
        "File"
    };
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_project_recent{
        lux::editor::commands::CommandIdView{"lux.editor.project.recent"},
        "Recent Projects",
        "File"
    };
}
namespace lux::editor::application
{
    void EditorApplication::Impl::installProjectTools(extensions::ContributionDraft& draft)
    {
        installRecentProjects(draft);
        installSettingsView(draft);
        installProjectCreation(draft);
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_initial_scene>(
            contracts::CodeLease::builtin(),
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

        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_project_open>(
            contracts::CodeLease::builtin(),
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

        draft.views.push_back(project::makeImportViewFactory(project_->catalogModel(), *importer_,
            [this](lux::ui::PaneId pane) { import_browse_ = std::move(pane); }
        ));
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_import>(
            contracts::CodeLease::builtin(),
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
    void EditorApplication::Impl::installRecentProjects(extensions::ContributionDraft& draft)
    {
        draft.views.push_back(project::makeRecentProjectsViewFactory(*recent_projects_,
            [this](const std::filesystem::path& path) -> EditorResult<void> {
                const bool is_unavailable = phase_ != EApplicationPhase::RUNNING ||
                    project_launch_.has_value() || project_launch_intent_.has_value();
                if (is_unavailable)
                    return cxx::unexpected(EditorFailure{EEditorError::BUSY, "recent.open"});
                project_launch_intent_ = path;
                return {};
            }
        ));
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_project_recent>(
            contracts::CodeLease::builtin(),
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
        draft.commands.push_back(commands::CommandEntry::create(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{
                commands::CommandIdView{"lux.editor.about"},
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
