#include <lux/engine/editor/application/EditorApplicationImpl.hpp>

namespace lux::editor::application
{
    void EditorApplication::Impl::installProjectCreation(extensions::ContributionDraft& draft)
    {
        draft.views.push_back(project::makeProjectCreationViewFactory(
            [this]
            {
                if (!project_creation_)
                    project_creation_ = std::make_unique<ProjectCreation>(
                        engine_->execution(),
                        messages_.dispatcherRef(),
                        config_.installation,
                        !config_.offscreen
                    );
                return project_creation_->requests();
            }
        ));
        draft.commands.push_back(project::makeProjectCreationCommand(
            [phase = &phase_](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{*phase == EApplicationPhase::RUNNING}; },
            toolOpening(),
            [creation = &project_creation_]() -> commands::CommandResult<void>
            {
                auto started = (*creation)->start();
                if (!started && started.error().code != EEditorError::BUSY)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        started.error().domain,
                        started.error().reason,
                        started.error().message
                    });
                return {};
            }
        ));
    }
} // namespace lux::editor::application
