#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/editor/project/RecentProjectsView.hpp>
#include <lux/engine/window/FileDialog.hpp>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor kAbout{
        lux::editor::commands::CommandIdView{"lux.editor.about"},
        "Lux Editor " LUX_EDITOR_VERSION,
        "Help"
    };
}
namespace lux::editor::application
{
    void EditorApplication::Impl::installProjectTools(extensions::ContributionDraft& draft)
    {
        installRecentProjects(draft);
        installSettingsView(draft);
        draft.commands.push_back(project::makeProjectCreationCommand(
            [phase = &phase_](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{*phase == EApplicationPhase::RUNNING}; },
            toolOpening()
        ));
        draft.commands.push_back(project::makeOpenProjectCommand(
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            {
                return commands::CommandState{
                    phase_ == EApplicationPhase::RUNNING && (!project_launching_ || !project_launching_->pending()) &&
                    !project_open_requested_ && !project_launch_intent_
                };
            },
            [this]() -> commands::CommandResult<void>
            {
                if ((project_launching_ && project_launching_->pending()) || project_open_requested_ ||
                    project_launch_intent_)
                {
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "project.open"});
                }
                project_open_requested_ = true;
                return {};
            }
        ));

        import_browse_request_ = [this](lux::ui::PaneHandle pane) { import_browse_ = std::move(pane); };
        draft.ui.push_back(desktop::UiEntry::bind<project::kImportView>(object::CodeLease::builtin()));
        draft.commands.push_back(project::makeImportCommand(
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
            toolOpening()
        ));
    }
    EditorResult<void> EditorApplication::Impl::receiveProjectIntents()
    {
        if (std::exchange(project_open_requested_, false))
        {
            const std::array filters{window::FileDialogFilter{"Lux project", "luxproject"}};
            auto selected = window::openFileDialog(window_.get(), filters);
            if (!selected)
            {
                return applicationFailure("project.dialog", selected.error());
            }
            if (*selected)
            {
                project_launch_intent_ = std::move(**selected);
            }
        }
        if (project_launch_intent_)
        {
            if (!project_launching_)
            {
                auto launching = editor_context_.services().get<ProjectLaunching>(editor_context_.scope());
                if (!launching)
                {
                    return applicationFailure("project.launch.service", launching.error());
                }
                project_launching_ = std::move(*launching);
            }
            auto accepted = project_launching_->request(*project_launch_intent_);
            if (!accepted)
            {
                return accepted;
            }
            project_launch_intent_.reset();
        }
        if (!import_browse_)
        {
            return {};
        }
        const auto target = std::exchange(import_browse_, {});
        auto original = desktop_->root().findPane(*target);
        if (!original)
        {
            return {}; // A closed UI cannot redirect its native result to a later window.
        }
        auto chosen = window::openFileDialog(window_.get());
        auto deliver = [&](lux::ui::Pane& pane)
        {
            auto& view = static_cast<project::ImportView&>(pane);
            if (!chosen)
            {
                view.showFailure(EditorFailure{EEditorError::SOURCE_FAILURE, "import.browse", 0, chosen.error().detail}
                );
            }
            else if (*chosen)
            {
                view.setSource(std::move(**chosen));
            }
        };
        auto delivered = desktop_->root().withPane(*target, deliver);
        if (!delivered)
        {
            return applicationFailure("import.browse.deliver", delivered.error());
        }
        return {};
    }
} // namespace lux::editor::application
namespace lux::editor::application
{
    void EditorApplication::Impl::installRecentProjects(extensions::ContributionDraft& draft)
    {
        recent_open_ = [this](const std::filesystem::path& path) -> EditorResult<void>
        {
            const bool is_unavailable = phase_ != EApplicationPhase::RUNNING ||
                                        (project_launching_ && project_launching_->pending()) ||
                                        project_launch_intent_.has_value();
            if (is_unavailable)
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "recent.open"});
            }
            project_launch_intent_ = path;
            return {};
        };
        draft.ui.push_back(desktop::UiEntry::bind<project::kRecentProjectsView>(object::CodeLease::builtin()));
        draft.commands.push_back(project::makeRecentProjectsCommand(
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
            toolOpening()
        ));
        draft.commands.push_back(commands::CommandEntry::bind<kAbout>(
            lux::object::CodeLease::builtin(),
            [](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{false}; },
            [](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>
            { return cxx::unexpected(commands::CommandFailure{commands::ECommandError::DISABLED}); }
        ));
    }
} // namespace lux::editor::application
