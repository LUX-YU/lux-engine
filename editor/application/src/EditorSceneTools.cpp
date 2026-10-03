#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/editor/scene/SceneConfigurationView.hpp>
#include <algorithm>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_new_scene{
        lux::editor::commands::CommandIdView{"lux.editor.new.scene"},
        "New Scene",
        "File"
    };
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_play{
        lux::editor::commands::CommandIdView{"lux.editor.play"},
        "Play Frozen Scene",
        "Scene",
        "Ctrl+P",
        lux::editor::commands::ECommandScope::SESSION
    };
}
namespace lux::editor::application
{
    namespace
    {
        commands::CommandFailure commandFailure(const EditorFailure& failure)
        {
            return {commands::ECommandError::DOMAIN_FAILURE, failure.domain, failure.reason, failure.message};
        }
    }
    EditorResult<scene::StartRunId> EditorApplication::Impl::play(commands::SessionTarget target)
    {
        if (phase_ != EApplicationPhase::RUNNING || run_presentations_.size() >= 16)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.admission"});
        auto information = sessions_.describe(target.id);
        if (!information)
            return applicationFailure("run.source", information.error());
        if (!target.based_on || information->current != *target.based_on)
            return applicationFailure("run.source", sessions::ESessionError::STALE_CONTENT);
        auto key = sessions_.key<scene::SceneSession>(target.id);
        if (!key)
            return applicationFailure("run.source.kind", key.error());
        auto author = sessions_.access<scene::SceneSession>().read(*key);
        if (!author)
            return applicationFailure("run.source.read", author.error());
        auto captured = author->get().capture();
        if (!captured)
            return applicationFailure("run.source.capture", captured.error());
        scene::RunConfiguration configuration;
        const auto& description = captured->configuration().scene->data();
        for (std::size_t i{}; i < description.systemCount(); ++i)
            if (const auto system = description.systemAt(i);
                system.type() == lux::scene::builtinRenderSystemRegistration().type)
            {
                if (configuration.viewport.value)
                    return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "run.viewport.ambiguous"});
                configuration.viewport = system.instanceId();
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
            return applicationFailure("run.prepare", prepared.error());
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
                    record.preparing->cancel();
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
                        return applicationFailure("run.info", information.error());
                    const auto name = "run-" + std::to_string(next_view_++);
                    auto candidate = scene::makeRunSceneView(
                        messages_.dispatcherRef(), sceneServices(), lux::ui::PaneId{name},
                        *adopted, information->provenance.configuration.viewport
                    );
                    if (!candidate)
                        record.failure = applicationFailure("run.view", candidate.error()).value();
                    else
                    {
                        auto shown = adopt(*candidate, name);
                        if (!shown)
                            record.failure = shown.error();
                        else
                        {
                            record.views.push_back(*shown);
                        }
                    }
                }
            }
            if ((phase_ == EApplicationPhase::DRAINING || record.stop_requested) && record.run && !record.stopping)
            {
                auto stopped = runs_.stop(*record.run);
                if (!stopped)
                    return applicationFailure("run.stop", stopped.error());
                record.stopping = *stopped;
            }
            // RunStore retains each original terminal result until the user's explicit Stop confirmation.
            // A frame must not manufacture acknowledgement merely because execution has finished.
            if (record.stopping && record.stopping->complete())
            {
                auto acknowledged = runs_.acknowledgeStop(*record.run);
                if (!acknowledged)
                    return applicationFailure("run.stop.acknowledge", acknowledged.error());
                iterator = run_presentations_.erase(iterator);
            }
            else
                ++iterator;
        }
        return {};
    }
    EditorResult<void> EditorApplication::Impl::stopRun(scene::RunId id)
    {
        auto record = std::ranges::find(run_presentations_, std::optional{id}, &RunPresentation::run);
        if (record == run_presentations_.end())
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "run.stop"});
        const auto ids = record->views;
        auto prepared = desktop_->views().prepareClose(ids);
        if (!prepared)
            return applicationFailure("run.views.prepare", prepared.error());
        auto stopped = runs_.stop(id);
        if (!stopped)
            return applicationFailure("run.stop", stopped.error());
        record->stopping = *stopped;
        auto committed = desktop_->views().commit(*prepared);
        if (!committed)
            return applicationFailure("run.views.close", committed.error());
        record->views.clear();
        return {};
    }
    EditorResult<views::ViewId> EditorApplication::Impl::showSceneTool(views::ViewId source, std::string_view role)
    {
        const auto kind = role == "outliner" ? scene::ESceneTool::OUTLINER
                        : role == "inspector" ? scene::ESceneTool::INSPECTOR
                        : role == "resources" ? scene::ESceneTool::RESOURCES
                        : scene::ESceneTool::CONFIGURATION;
        auto source_group = scene::shareSceneInteraction(desktop_->views(), source);
        if (!source_group)
            return applicationFailure("scene.tool.source", source_group.error());
        const auto run = (*source_group)->run();
        const auto snapshot = contributions_.snapshot();
        auto components = scene::sceneInspectorComponents();
        components.insert(components.end(), snapshot.components().begin(), snapshot.components().end());
        const auto name = std::string(role) + "-" + std::to_string(next_view_++);
        auto candidate = scene::makeSceneToolView(
            messages_.dispatcherRef(), lux::ui::PaneId{name}, desktop_->views(), source, kind,
            {sceneServices(), runs_, registrations_.components, std::move(components),
             &project_->catalogModel(), sceneConfigurationInputs()}
        );
        if (!candidate)
            return applicationFailure("scene.tool.create", candidate.error());
        auto shown = adopt(*candidate, name);
        if (!shown)
            return shown;
        if (run)
        {
            auto owner = std::ranges::find(run_presentations_, run, &RunPresentation::run);
            if (owner != run_presentations_.end())
                owner->views.push_back(*shown);
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
            ) -> scene::SceneConfigurationResult<scene::ConfigurationControl> {
                for (const auto& editor : snapshot.configurations())
                    if (editor.value.schema_name == name && editor.value.schema_version == version)
                        return scene::makeConfigurationControl(editor, parent, lux::ui::ElementId{name}, initial);
                return scene::ConfigurationControl{};
            },
            registrations_.render_bindings
        };
    }
    void EditorApplication::Impl::installSceneCommands(extensions::ContributionDraft& draft)
    {
        draft.views.push_back(extensions::builtinSceneCreationFactory(sceneConfigurationInputs(), contentCreation()));
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_new_scene>(
            contracts::CodeLease::builtin(),
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto shown = showTool(views::ViewTypeId{"lux.editor.scene.creation"});
                if (!shown)
                    return cxx::unexpected(commandFailure(shown.error()));
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        draft.commands.push_back(commands::CommandEntry::bind<command_lux_editor_play>(
            contracts::CodeLease::builtin(),
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation& invocation
            ) -> commands::CommandResult<commands::DispatchReceipt> {
                auto started = play(std::get<commands::SessionTarget>(invocation.target()));
                if (!started)
                    return cxx::unexpected(commandFailure(started.error()));
                return commands::DispatchReceipt{commands::AcceptedOperation{"run", started->serial}};
            }
        ));
        for (const auto role :
             {"outliner", "inspector", "resources", "configuration", "pause", "resume", "step", "stop"})
            draft.commands.push_back(commands::CommandEntry::create(
                contracts::CodeLease::builtin(),
                commands::CommandDescriptor{
                    commands::CommandIdView{std::string("lux.editor.scene.") + role},
                    role,
                    "Scene",
                    "",
                    commands::ECommandScope::VIEW
                },
                [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                    return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
                },
                [this, role = std::string(role)](const commands::CommandInvocation& invocation
                ) -> commands::CommandResult<commands::DispatchReceipt> {
                    const auto view = std::get<views::ViewId>(invocation.target());
                    if (role == "outliner" || role == "inspector" || role == "resources" || role == "configuration")
                    {
                        auto shown = showSceneTool(view, role);
                        if (!shown)
                            return cxx::unexpected(commandFailure(shown.error()));
                    }
                    else
                    {
                        auto group = scene::shareSceneInteraction(desktop_->views(), view);
                        const auto run = group ? (*group)->run() : std::optional<scene::RunId>{};
                        if (!group)
                            return cxx::unexpected(commandFailure(applicationFailure("run.view", group.error()).value()));
                        if (!run)
                            return cxx::unexpected(
                                commands::CommandFailure{commands::ECommandError::STALE_TARGET, "run.view"}
                            );
                        if (role == "stop")
                        {
                            auto result = stopRun(*run);
                            if (!result)
                                return cxx::unexpected(commandFailure(result.error()));
                        }
                        else if (role == "step")
                        {
                            auto owner = std::ranges::find(run_presentations_, run, &RunPresentation::run);
                            if (owner == run_presentations_.end() || owner->steps.size() >= 64)
                                return cxx::unexpected(
                                    commands::CommandFailure{commands::ECommandError::BUSY, "run.steps"}
                                );
                            auto step = runs_.step(*run);
                            if (!step)
                                return cxx::unexpected(
                                    commandFailure(applicationFailure("run.step", step.error()).value())
                                );
                            owner->steps.push_back(*step);
                        }
                        else
                        {
                            auto result = role == "pause" ? runs_.pause(*run) : runs_.resume(*run);
                            if (!result)
                                return cxx::unexpected(
                                    commandFailure(applicationFailure("run.control", result.error()).value())
                                );
                        }
                    }
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            ));
    }
}
