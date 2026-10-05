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
    EditorResult<scene::StartRunId> EditorApplication::Impl::play(commands::SessionTarget target)
    {
        if (phase_ != EApplicationPhase::RUNNING || run_presentations_.size() >= 16)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.admission"});
        }
        auto information = sessions_->describe(target.id);
        if (!information)
        {
            return applicationFailure("run.source", information.error());
        }
        if (!target.based_on || information->current != *target.based_on)
        {
            return applicationFailure("run.source", sessions::ESessionError::STALE_CONTENT);
        }
        auto key = sessions_->key<scene::SceneSession>(target.id);
        if (!key)
        {
            return applicationFailure("run.source.kind", key.error());
        }
        auto author = sessions_->access<scene::SceneSession>().read(*key);
        if (!author)
        {
            return applicationFailure("run.source.read", author.error());
        }
        auto captured = author->get().capture();
        if (!captured)
        {
            return applicationFailure("run.source.capture", captured.error());
        }
        scene::RunConfiguration configuration;
        const auto& description = captured->configuration().scene->data();
        for (std::size_t i{}; i < description.systemCount(); ++i)
        {
            if (const auto system = description.systemAt(i);
                system.type() == lux::scene::builtinRenderSystemRegistration().type)
            {
                if (configuration.viewport.value)
                {
                    return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "run.viewport.ambiguous"});
                }
                configuration.viewport = system.instanceId();
            }
        }
        auto environment = editor_context_.services().get<scene::ProjectionEnvironment>(editor_context_.scope());
        if (!environment)
        {
            return applicationFailure("run.environment", environment.error());
        }
        if (!runs_)
        {
            auto runs = editor_context_.services().get<scene::RunStore>(editor_context_.scope());
            if (!runs)
            {
                return applicationFailure("run.service", runs.error());
            }
            runs_ = std::move(*runs);
        }
        // The preparation owns the frozen author data and this exact environment, never a live Session.
        auto prepared = runs_->prepare(
            std::move(*captured),
            {(*environment)->components,
             (*environment)->simulation_systems,
             (*environment)->scene_systems,
             (*environment)->render_bindings,
             (*environment)->renderer,
             (*environment)->resources,
             (*environment)->assets},
            configuration
        );
        if (!prepared)
        {
            return applicationFailure("run.prepare", prepared.error());
        }
        const auto id = (*prepared)->id();
        run_presentations_.push_back({id, information->current, std::move(*prepared)});
        return id;
    }
    EditorResult<void> EditorApplication::Impl::maintainRuns()
    {
        for (auto iterator = run_presentations_.begin(); iterator != run_presentations_.end();)
        {
            auto& record = *iterator;
            if (record.preparing)
            {
                if (phase_ == EApplicationPhase::DRAINING)
                {
                    record.preparing->cancel();
                }
                if (!record.preparing->ready() || phase_ == EApplicationPhase::REVIEWING)
                {
                    ++iterator;
                    continue;
                }
                auto adopted = runs_->adopt(*record.preparing);
                if (!adopted)
                {
                    const auto* control = std::get_if<scene::ERunError>(&adopted.error().cause);
                    if (control && *control == scene::ERunError::BUSY)
                    {
                        ++iterator;
                        continue;
                    }
                    record.failure = applicationFailure("run.adopt", adopted.error()).value();
                    record.preparing.reset();
                    ++iterator;
                    continue;
                }
                record.preparing.reset();
                record.run = *adopted;
                if (phase_ != EApplicationPhase::DRAINING)
                {
                    const auto information = runs_->info(*adopted);
                    if (!information)
                    {
                        return applicationFailure("run.info", information.error());
                    }
                    const auto name = "run-" + std::to_string(next_view_++);
                    auto factory = contributions_.snapshot().ui().find(scene::kSceneView.type);
                    if (!factory)
                    {
                        return applicationFailure("run.factory", factory.error());
                    }
                    auto candidate = editor_context_.ui().create(
                        *factory,
                        editor_context_.scope(),
                        {messages_.dispatcherRef(),
                         lux::ui::PaneId{name},
                         {},
                         {factory->descriptor().schema, {}},
                         views::ViewRestoreKey{name}}
                    );
                    if (!candidate)
                    {
                        record.failure = applicationFailure("run.view", candidate.error()).value();
                    }
                    else
                    {
                        auto& view = static_cast<scene::SceneView&>(**candidate);
                        view.setTitle("Run (frozen author content)");
                        auto bound = view.rebindRun(*adopted, information->provenance.configuration.viewport);
                        if (!bound)
                        {
                            record.failure = applicationFailure("run.view.bind", bound.error()).value();
                        }
                        else
                        {
                            auto shown = adopt(*candidate);
                            if (!shown)
                            {
                                record.failure = shown.error();
                            }
                            else
                            {
                                record.views.push_back(*shown);
                            }
                        }
                    }
                }
            }
            if ((phase_ == EApplicationPhase::DRAINING || record.stop_requested) && record.run && !record.stopping)
            {
                auto stopped = runs_->stop(*record.run);
                if (!stopped)
                {
                    return applicationFailure("run.stop", stopped.error());
                }
                record.stopping = *stopped;
            }
            // RunStore retains each original terminal result until the user's explicit Stop confirmation.
            // A frame must not manufacture acknowledgement merely because execution has finished.
            if (record.stopping && record.stopping->complete())
            {
                auto acknowledged = runs_->acknowledgeStop(*record.run);
                if (!acknowledged)
                {
                    // Retirement may finish after this frame's RunStore maintenance. Keep the
                    // result until that owner has adopted it; never drive the Runtime again here.
                    const auto* control = std::get_if<scene::ERunError>(&acknowledged.error().cause);
                    if (control && *control == scene::ERunError::BUSY)
                    {
                        ++iterator;
                        continue;
                    }
                    return applicationFailure("run.stop.acknowledge", acknowledged.error());
                }
                iterator = run_presentations_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        return {};
    }
    EditorResult<void> EditorApplication::Impl::stopRun(scene::RunId id)
    {
        auto record = std::ranges::find(run_presentations_, std::optional{id}, &RunPresentation::run);
        if (record == run_presentations_.end())
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "run.stop"});
        }
        const auto ids = record->views;
        auto prepared = editor_context_.ui().prepareClose(desktop_->root(), ids);
        if (!prepared)
        {
            return applicationFailure("run.views.prepare", prepared.error());
        }
        auto stopped = runs_->stop(id);
        if (!stopped)
        {
            return applicationFailure("run.stop", stopped.error());
        }
        record->stopping = *stopped;
        auto committed = desktop_->root().commit(*prepared);
        if (!committed)
        {
            return applicationFailure("run.views.close", committed.error());
        }
        record->views.clear();
        return {};
    }
    EditorResult<lux::ui::PaneHandle> EditorApplication::Impl::showSceneTool(
        lux::ui::PaneHandle source,
        scene::ESceneTool kind
    )
    {
        auto source_group = scene::shareSceneInteraction(desktop_->root(), source);
        if (!source_group)
        {
            return applicationFailure("scene.tool.source", source_group.error());
        }
        const auto run = (*source_group)->run();
        auto candidate = scene::createSceneTool(
            editor_context_.ui(),
            editor_context_.services(),
            editor_context_.scope(),
            desktop_->root(),
            source,
            kind,
            lux::ui::PaneId{"scene-tool-" + std::to_string(next_view_++)}
        );
        if (!candidate)
        {
            return applicationFailure("scene.tool.create", candidate.error());
        }
        auto shown = adopt(*candidate);
        if (!shown)
        {
            return shown;
        }
        if (run)
        {
            auto owner = std::ranges::find(run_presentations_, run, &RunPresentation::run);
            if (owner != run_presentations_.end())
            {
                owner->views.push_back(*shown);
            }
        }
        return *shown;
    }
    EditorResult<void> EditorApplication::Impl::stepRun(scene::RunId run)
    {
        auto owner = std::ranges::find(run_presentations_, std::optional{run}, &RunPresentation::run);
        const bool is_full = owner == run_presentations_.end() || owner->steps.size() >= 64;
        if (is_full)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.steps"});
        }
        auto step = runs_->step(run);
        if (!step)
        {
            return applicationFailure("run.step", step.error());
        }
        owner->steps.push_back(*step);
        return {};
    }
    void EditorApplication::Impl::installSceneCommands(extensions::ContributionDraft& draft)
    {
        const auto available = [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
        { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; };
        draft.commands.push_back(scene::makeNewSceneCommand(available, toolOpening()));
        draft.commands.push_back(scene::makePlaySceneCommand(
            available,
            [this](commands::SessionTarget target) -> commands::CommandResult<scene::StartRunId>
            {
                auto started = play(target);
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
                auto shown = showSceneTool(view, kind);
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
                if (!runs_)
                {
                    return cxx::unexpected(scene::RunFailure{scene::ERunError::INVALID_ID});
                }
                return paused ? runs_->pause(id) : runs_->resume(id);
            },
            [this](scene::RunId id) -> commands::CommandResult<void>
            {
                auto result = stepRun(id);
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
                auto result = stopRun(id);
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
