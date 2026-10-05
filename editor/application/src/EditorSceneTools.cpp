#include <algorithm>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/scene/SceneConfigurationView.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/scene/RenderSystem.hpp>

namespace lux::editor::application
{
    namespace
    {
        commands::CommandFailure commandFailure(const EditorFailure& failure)
        {
            return {commands::ECommandError::DOMAIN_FAILURE, failure.domain, failure.reason, failure.message};
        }
    } // namespace
    void EditorApplication::Impl::installSceneCommands(extensions::ContributionDraft& draft)
    {
        const auto available = [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
        { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; };
        draft.commands.push_back(scene::makeNewSceneCommand(available, toolOpening()));
        draft.commands.push_back(scene::makePlaySceneCommand(
            available,
            [this](commands::SessionTarget target) -> commands::CommandResult<scene::StartRunId>
            {
                if (!target.based_on || target.based_on->session != target.id)
                {
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE, "run.source"
                    });
                }
                auto service = editor_context_.services().get<scene::ScenePlayback>(editor_context_.scope());
                if (!service)
                {
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE, "run.service"
                    });
                }
                playback_ = std::move(*service);
                auto started = playback_->play(*target.based_on);
                if (!started)
                {
                    return cxx::unexpected(commandFailure(started.error()));
                }
                return *started;
            }
        ));
        auto tools = scene::makeSceneToolCommands(
            available,
            [this](lux::ui::PaneHandle view, scene::ESceneTool kind) -> commands::CommandResult<void>
            {
                auto service = editor_context_.services().get<scene::ScenePlayback>(editor_context_.scope());
                if (!service)
                {
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE, "scene.tool.service"
                    });
                }
                playback_ = std::move(*service);
                auto shown = playback_->showTool(view, kind);
                if (!shown)
                {
                    return cxx::unexpected(commandFailure(shown.error()));
                }
                return {};
            }
        );
        auto runs = scene::makeRunViewCommands(
            available,
            desktop_->root(),
            [this](scene::RunId id, bool paused) -> scene::RunResult<void>
            {
                auto runs = editor_context_.services().get<scene::RunStore>(editor_context_.scope());
                if (!runs)
                {
                    return cxx::unexpected(scene::RunFailure{scene::ERunError::INVALID_ID});
                }
                return paused ? (*runs)->pause(id) : (*runs)->resume(id);
            },
            [this](scene::RunId id) -> commands::CommandResult<void>
            {
                if (!playback_)
                {
                    return cxx::unexpected(commandFailure(EditorFailure{EEditorError::STALE_REQUEST, "run.steps"}));
                }
                auto result = playback_->step(id);
                if (!result)
                {
                    if (result.error().code == EEditorError::BUSY)
                    {
                        return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "run.steps"});
                    }
                    return cxx::unexpected(commandFailure(result.error()));
                }
                return {};
            },
            [this](scene::RunId id) -> commands::CommandResult<void>
            {
                if (!playback_)
                {
                    return cxx::unexpected(commandFailure(EditorFailure{EEditorError::STALE_REQUEST, "run.stop"}));
                }
                auto result = playback_->stop(id);
                if (!result)
                {
                    return cxx::unexpected(commandFailure(result.error()));
                }
                return {};
            }
        );
        draft.commands.insert(draft.commands.end(), tools.begin(), tools.end());
        draft.commands.insert(draft.commands.end(), runs.begin(), runs.end());
    }
} // namespace lux::editor::application

namespace lux::editor::application
{
    void EditorApplication::Impl::receiveModel(scene::ModelPlacement placement)
    {
        if (phase_ != EApplicationPhase::RUNNING)
        {
            result_failure_ = EditorFailure{EEditorError::CLOSING, "model.admission"};
            return;
        }
        if (!model_placements_)
        {
            auto service = editor_context_.services().get<scene::ModelPlacementService>(editor_context_.scope());
            if (!service)
            {
                EditorResult<void> failure = applicationFailure("model.service", service.error());
                result_failure_ = std::move(failure.error());
                return;
            }
            model_placements_ = std::move(*service);
        }
        auto request = model_placements_->request(std::move(placement));
        if (!request)
        {
            result_failure_ = std::move(request.error());
        }
    }
} // namespace lux::editor::application
