#include <lux/engine/editor/application/EditorApplicationImpl.hpp>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_project_create{
        lux::editor::commands::CommandIdView{"lux.editor.project.create"},
        "New Project",
        "File"
    };
}
namespace lux::editor::application
{
    void EditorApplication::Impl::installProjectCreation(extensions::ContributionDraft& draft)
    {
        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.project.creation"},
                "New project",
                cxx::typeToken<std::monostate>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                if (!project_creation_)
                    project_creation_ = std::make_unique<ProjectCreation>(
                        engine_->execution(),
                        messages_.dispatcherRef(),
                        config_.installation,
                        !config_.offscreen
                    );
                EditorResult<void> ready;
                auto pane = std::make_unique<project::ProjectCreationView>(
                    input.dispatcher(),
                    input.paneId(),
                    project_creation_->requests(),
                    ready
                );
                if (!ready)
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT,
                        ready.error().domain,
                        ready.error().reason,
                        ready.error().message
                    });
                return views::DetachedView{contracts::CodeLease::builtin(), std::move(pane)};
            }
        ));
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_project_create>(
            contracts::CodeLease::builtin(),
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto shown = showTool(views::ViewTypeId{"lux.editor.project.creation"});
                if (!shown)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        shown.error().domain,
                        shown.error().reason,
                        shown.error().message
                    });
                // Complete detached construction/mount precedes starting any task requiring maintenance.
                auto started = project_creation_->start();
                if (!started && started.error().code != EEditorError::BUSY)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        started.error().domain,
                        started.error().reason,
                        started.error().message
                    });
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
    }
}
