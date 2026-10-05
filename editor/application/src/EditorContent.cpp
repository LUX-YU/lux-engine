#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>

namespace lux::editor::application
{
    void EditorApplication::Impl::installContentCommands(extensions::ContributionDraft& draft)
    {
        const auto running = [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
        { return commands::CommandState{phase_ == EApplicationPhase::RUNNING && !last_view_}; };
        draft.commands.push_back(desktop::makeCloseViewCommand(
            running,
            [this](lux::ui::PaneHandle id) -> commands::CommandResult<void>
            {
                auto result = closeView(id);
                if (!result)
                {
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        result.error().domain,
                        result.error().reason,
                        result.error().message
                    });
                }
                return {};
            }
        ));
        draft.commands.push_back(desktop::makeAnotherViewCommand(
            running,
            [this](commands::SessionTarget target) -> commands::CommandResult<void>
            {
                auto result = content_views_->show(target.id, true);
                if (!result)
                {
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        result.error().domain,
                        result.error().reason,
                        result.error().message
                    });
                }
                return {};
            }
        ));
        draft.commands.push_back(project::makeAssetsCommand(running, toolOpening()));
        draft.commands.push_back(tasks::makeTasksCommand(running, toolOpening()));
    }
    sessions::SessionCreation EditorApplication::Impl::contentCreation()
    {
        return [this](sessions::SessionPreparation prepared) -> commands::CommandResult<commands::DispatchReceipt>
        {
            auto opened = content_views_->create(std::move(prepared));
            if (!opened)
            {
                const auto& error = opened.error();
                const auto code = error.code == EEditorError::BUSY ? commands::ECommandError::BUSY
                                                                   : commands::ECommandError::DOMAIN_FAILURE;
                return cxx::unexpected(commands::CommandFailure{code, error.domain, error.reason, error.message});
            }
            return commands::DispatchReceipt{
                commands::AcceptedOperation{commands::OperationKindId{"open"}, opened->value}
            };
        };
    }
} // namespace lux::editor::application
