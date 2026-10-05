#include <algorithm>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/scene/SceneConfigurationView.hpp>
#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>
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
        auto information = sessions_.describe(target.id);
        if (!information)
        {
            return applicationFailure("run.source", information.error());
        }
        if (!target.based_on || information->current != *target.based_on)
        {
            return applicationFailure("run.source", sessions::ESessionError::STALE_CONTENT);
        }
        auto key = sessions_.key<scene::SceneSession>(target.id);
        if (!key)
        {
            return applicationFailure("run.source.kind", key.error());
        }
        auto author = sessions_.access<scene::SceneSession>().read(*key);
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
        // The preparation owns the frozen author data and this exact environment, never a live Session.
        auto prepared = run_controller_.prepare(
            std::move(*captured),
            {environment_.components,
             environment_.simulation_systems,
             environment_.scene_systems,
             environment_.render_bindings,
             environment_.renderer,
             environment_.resources,
             environment_.assets},
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
                auto adopted = run_controller_.adopt(*record.preparing);
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
                    const auto information = runs_.info(*adopted);
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
                auto stopped = runs_.stop(*record.run);
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
                auto acknowledged = runs_.acknowledgeStop(*record.run);
                if (!acknowledged)
                {
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
        auto stopped = runs_.stop(id);
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
        const desktop::UiDescriptor* descriptor{};
        switch (kind)
        {
        case scene::ESceneTool::OUTLINER:
            descriptor = &scene::kOutlinerView;
            break;
        case scene::ESceneTool::INSPECTOR:
            descriptor = run ? &scene::kRunInspectorView : &scene::kInspectorView;
            break;
        case scene::ESceneTool::RESOURCES:
            descriptor = &scene::kResourceView;
            break;
        case scene::ESceneTool::CONFIGURATION:
            descriptor = &scene::kSceneConfigurationView;
            break;
        }
        if (!descriptor)
        {
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.tool.kind"});
        }
        auto scope = editor_context_.services().createScope(&editor_context_.scope());
        if (!scope)
        {
            return applicationFailure("scene.tool.scope", scope.error());
        }
        if (auto provided = scope->provide(services::ServiceNameView{"lux.editor.scene.interaction"}, *source_group);
            !provided)
        {
            return applicationFailure("scene.tool.group", provided.error());
        }
        if (auto provided = scope->provide(services::ServiceNameView{"lux.ui.root"}, desktop_->root()); !provided)
        {
            return applicationFailure("scene.tool.root", provided.error());
        }
        if (auto provided = scope->provide(services::ServiceNameView{"lux.editor.scene.viewport"}, source); !provided)
        {
            return applicationFailure("scene.tool.viewport", provided.error());
        }
        views::ViewContent content;
        if (auto session = (*source_group)->session())
        {
            content = {{session->id()}, session->id()};
        }
        const auto name = "scene-tool-" + std::to_string(next_view_++);
        auto factory = contributions_.snapshot().ui().find(descriptor->type);
        if (!factory)
        {
            return applicationFailure("scene.tool.factory", factory.error());
        }
        auto candidate = editor_context_.ui().create(
            *factory,
            *scope,
            {messages_.dispatcherRef(),
             lux::ui::PaneId{name},
             content,
             {factory->descriptor().schema, {}},
             views::ViewRestoreKey{name}}
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
    scene::SceneConfigurationInputs EditorApplication::Impl::sceneConfigurationInputs()
    {
        static constexpr scene::SceneProviderOption providers[]{
            {"lux.render.runtime", "main-window"},
            {"lux.render.scene_bindings", "render-bindings"},
            {"lux.render.resources", "resources"},
            {"lux.render.assets", "assets"},
            {"lux.world.loading", "world-storage"}
        };
        auto snapshot = contributions_.snapshot();
        return {
            plugins_.catalog(),
            registrations_.components,
            *registrations_.simulation_systems,
            registrations_.scene_systems,
            registrations_.features,
            providers,
            [snapshot = std::move(snapshot)](
                lux::ui::Element& parent,
                std::string_view name,
                std::uint32_t version,
                const serialization::PortableValueCodec&,
                std::optional<std::span<const std::byte>> initial
            ) -> scene::SceneConfigurationResult<scene::ConfigurationControl>
            {
                auto definitions = scene::sceneEditorDefinitions(snapshot.services());
                if (!definitions)
                {
                    return cxx::unexpected(scene::SceneConfigurationFailure{
                        scene::ESceneConfigurationError::CONTROL_FAILURE,
                        "configuration.catalog"
                    });
                }
                for (const auto& definition : *definitions)
                {
                    for (const auto& editor : definition->configurations)
                    {
                        if (editor.value.schema_name == name && editor.value.schema_version == version)
                        {
                            return scene::makeConfigurationControl(editor, parent, lux::ui::ElementId{name}, initial);
                        }
                    }
                }
                return scene::ConfigurationControl{};
            },
            registrations_.render_bindings
        };
    }
    EditorResult<void> EditorApplication::Impl::stepRun(scene::RunId run)
    {
        auto owner = std::ranges::find(run_presentations_, std::optional{run}, &RunPresentation::run);
        const bool is_full = owner == run_presentations_.end() || owner->steps.size() >= 64;
        if (is_full)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.steps"});
        }
        auto step = runs_.step(run);
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
            runs_,
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
