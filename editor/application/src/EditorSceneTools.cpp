#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/editor/scene/RunInspectorView.hpp>
#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/scene/ResourceView.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/editor/scene/SceneConfigurationView.hpp>
#include <random>
#include <algorithm>

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
                record.interaction = std::make_shared<scene::SceneInteractionGroup>(
                    runs_.inspect(),
                    *adopted,
                    scene::InteractionGroupId{next_view_}
                );
                if (phase_ != EApplicationPhase::DRAINING)
                {
                    const auto information = runs_.info(*adopted);
                    if (!information)
                        return applicationFailure("run.info", information.error());
                    const auto name = "run-" + std::to_string(next_view_++);
                    scene::SceneViewCreateInfo input;
                    input.id = lux::ui::PaneId{name};
                    input.title = "Run (frozen author content)";
                    input.binding = scene::RunningSceneBinding{*adopted, record.interaction.get()};
                    input.state.camera.transform.translation = {0, 3, 8};
                    input.render_system = information->provenance.configuration.viewport;
                    auto candidate = scene::makeSceneView(messages_.dispatcherRef(), sceneServices(), std::move(input));
                    if (!candidate)
                        record.failure = applicationFailure("run.view", candidate.error()).value();
                    else
                    {
                        auto shown = adopt(*candidate, name);
                        if (!shown)
                            record.failure = shown.error();
                        else
                        {
                            ContentView content;
                            content.view = *shown;
                            content.scene = record.interaction;
                            content.run = *adopted;
                            content_views_.push_back(std::move(content));
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
        std::vector<views::ViewId> ids;
        for (const auto& view : content_views_)
            if (view.run == id)
                ids.push_back(view.view);
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
        std::erase_if(content_views_, [id](const auto& view) { return view.run == id; });
        return {};
    }
    EditorResult<views::ViewId> EditorApplication::Impl::showSceneTool(views::ViewId source, std::string_view role)
    {
        const auto found = std::ranges::find(content_views_, source, &ContentView::view);
        if (found == content_views_.end() || !found->scene)
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.tool.source"});
        ContentView record;
        record.session = found->session;
        record.scene = found->scene;
        record.run = found->run;
        record.source_view = source;
        const auto name = std::string(role) + "-" + std::to_string(next_view_++);
        const auto pane = lux::ui::PaneId{name};
        scene::VSceneViewBinding binding =
            record.run
                ? scene::VSceneViewBinding{scene::RunningSceneBinding{*record.run, record.scene.get()}}
                : scene::VSceneViewBinding{scene::EditedSceneBinding{*record.scene->session(), record.scene.get()}};
        std::optional<views::DetachedView> candidate;
        if (role == "outliner")
        {
            auto built = scene::makeOutlinerView(
                messages_.dispatcherRef(),
                pane,
                sessions_.access<scene::SceneSession>(),
                binding,
                runs_.inspect(),
                registrations_.components
            );
            if (!built)
                return applicationFailure("outliner.create", built.error());
            candidate.emplace(std::move(*built));
        }
        else if (role == "inspector")
        {
            const auto& selection = record.scene->selection().objects;
            if (selection.empty())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "inspector.selection"});
            if (record.run)
            {
                const auto* target = std::get_if<scene::RunningObjectRef>(&selection.front());
                if (!target)
                    return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "inspector.run"});
                auto built = scene::makeRunInspectorView(
                    messages_.dispatcherRef(),
                    pane,
                    runs_,
                    *target,
                    registrations_.components,
                    scene::runInspectorComponents(),
                    &project_->catalogModel()
                );
                if (!built)
                    return applicationFailure("inspector.run", built.error());
                candidate.emplace(std::move(*built));
            }
            else
            {
                const auto* target = std::get_if<scene::SceneObjectRef>(&selection.front());
                if (!target)
                    return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "inspector.author"});
                // This private entry is called by CommandRegistry::execute, whose active dispatch
                // already excludes compound contribution publication. Pin its immutable catalog;
                // acquiring a second command batch would reject this command itself.
                const auto snapshot = contributions_.snapshot();
                auto components = scene::sceneInspectorComponents();
                components.insert(components.end(), snapshot.components().begin(), snapshot.components().end());
                auto built = scene::makeInspectorView(
                    messages_.dispatcherRef(),
                    pane,
                    sessions_.access<scene::SceneSession>(),
                    std::get<scene::EditedSceneBinding>(binding),
                    *target,
                    registrations_.components,
                    std::move(components),
                    &project_->catalogModel()
                );
                if (!built)
                    return applicationFailure("inspector.author", built.error());
                candidate.emplace(std::move(*built));
            }
        }
        else if (role == "configuration")
        {
            if (record.run || !record.scene->session())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "configuration.author"});
            auto built = scene::makeSceneConfigurationView(
                messages_.dispatcherRef(),
                pane,
                sessions_.access<scene::SceneSession>(),
                sceneConfigurationInputs(),
                *record.scene->session()
            );
            if (!built)
                return applicationFailure("configuration.create", built.error());
            candidate.emplace(std::move(*built));
        }
        else if (role == "resources")
        {
            std::optional<scene::ResourceViewBinding> target;
            auto read = [&](lux::ui::Pane& view) {
                if (view.type() == lux::ui::PaneTypeId{"lux.editor.scene.view"})
                {
                    auto instance = static_cast<scene::SceneView&>(view).presentedInstance();
                    if (instance.valid())
                    {
                        // The exact RenderSystem is the one bound by this viewport, never a first-match lookup.
                        auto system = static_cast<scene::SceneView&>(view).renderSystem();
                        target = scene::ResourceViewBinding{instance, system};
                    }
                }
            };
            auto read_result = desktop_->views().withView(source, read);
            if (!read_result)
                return applicationFailure("resource.source", read_result.error());
            auto built = scene::makeResourceView(messages_.dispatcherRef(), pane, engine_->sceneRuntime(), target);
            if (!built)
                return applicationFailure("resources.create", built.error());
            candidate.emplace(std::move(*built));
        }
        else
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.tool.role"});
        auto shown = adopt(*candidate, name);
        if (!shown)
            return shown;
        record.view = *shown;
        content_views_.push_back(std::move(record));
        return *shown;
    }
    EditorResult<void> EditorApplication::Impl::synchronizeSceneTools()
    {
        for (const auto& record : content_views_)
        {
            if (!record.scene || !record.source_view.valid())
                continue;
            const auto& selected = record.scene->selection().objects;
            std::optional<EditorFailure> error;
            std::optional<scene::ResourceViewBinding> resource;
            // Complete this borrow before entering the auxiliary view: Host callbacks cannot nest.
            auto source = [&](lux::ui::Pane& pane) {
                if (pane.type() == lux::ui::PaneTypeId{"lux.editor.scene.view"})
                {
                    auto& viewport = static_cast<scene::SceneView&>(pane);
                    if (viewport.presentedInstance().valid())
                        resource = scene::ResourceViewBinding{viewport.presentedInstance(), viewport.renderSystem()};
                }
            };
            auto observed = desktop_->views().withView(record.source_view, source);
            if (!observed)
                return applicationFailure("scene.tool.source", observed.error());
            auto synchronize = [&](lux::ui::Pane& pane) {
                if (pane.type() == lux::ui::PaneTypeId{"lux.editor.inspector"})
                {
                    const auto* target =
                        selected.empty() ? nullptr : std::get_if<scene::SceneObjectRef>(&selected.front());
                    auto& inspector = static_cast<scene::InspectorView&>(pane);
                    if (!target && inspector.target())
                    {
                        auto cleared = inspector.clearTarget();
                        if (!cleared)
                            error = applicationFailure("inspector.clear", cleared.error()).value();
                    }
                    if (target && inspector.target() != *target)
                    {
                        auto bound = inspector.rebind({*record.scene->session(), record.scene.get()}, *target);
                        if (!bound)
                            error = applicationFailure("inspector.rebind", bound.error()).value();
                    }
                }
                else if (pane.type() == lux::ui::PaneTypeId{"lux.editor.run-inspector"})
                {
                    const auto* target =
                        selected.empty() ? nullptr : std::get_if<scene::RunningObjectRef>(&selected.front());
                    auto& inspector = static_cast<scene::RunInspectorView&>(pane);
                    if (!target && inspector.target())
                    {
                        auto cleared = inspector.clearTarget();
                        if (!cleared)
                            error = applicationFailure("run-inspector.clear", cleared.error()).value();
                    }
                    if (target && inspector.target() != *target)
                    {
                        auto bound = inspector.rebind(*target);
                        if (!bound)
                            error = applicationFailure("run-inspector.rebind", bound.error()).value();
                    }
                }
                else if (pane.type() == lux::ui::PaneTypeId{"lux.editor.resources"})
                {
                    auto rebound = static_cast<scene::ResourceView&>(pane).rebind(resource);
                    if (!rebound)
                        error = applicationFailure("resources.rebind", rebound.error()).value();
                }
            };
            auto read = desktop_->views().withView(record.view, synchronize);
            if (!read)
                return applicationFailure("inspector.view", read.error());
            if (error)
                return cxx::unexpected(std::move(*error));
        }
        return {};
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
            }
        };
    }
    void EditorApplication::Impl::installSceneCommands(extensions::ContributionDraft& draft)
    {
        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.scene.creation"},
                "New Scene",
                cxx::typeToken<EmptyViewInput>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                scene::SceneCreationRequests requests{
                    [this](const scene::SceneCreationConfiguration& value) -> scene::SceneConfigurationResult<void> {
                        std::mt19937 random{std::random_device{}()};
                        auto package = lux::scene::createScenePackage(
                            asset::AssetId{uuids::uuid_random_generator{random}()},
                            value.name,
                            value.schemas,
                            value.simulation,
                            value.scene
                        );
                        if (!package)
                            return cxx::unexpected(scene::SceneConfigurationFailure{
                                scene::ESceneConfigurationError::CONTROL_FAILURE,
                                "scene.creation.package",
                                0,
                                {},
                                std::any{package.error()}
                            });
                        auto prepared =
                            scene::prepareSceneSession({std::move(*package)}, {}, {}, registrations_.components);
                        auto installed = createContent(std::move(prepared));
                        if (!installed)
                        {
                            log::error("application.scene.create", "{}", installed.error().domain);
                            return cxx::unexpected(scene::SceneConfigurationFailure{
                                installed.error().code == EEditorError::BUSY
                                    ? scene::ESceneConfigurationError::BUSY
                                    : scene::ESceneConfigurationError::CONTROL_FAILURE,
                                installed.error().domain,
                                installed.error().reason,
                                installed.error().message,
                                std::any{installed.error()}
                            });
                        }
                        return {};
                    }
                };
                auto created = scene::makeSceneCreationView(
                    input.dispatcher(),
                    input.paneId(),
                    sceneConfigurationInputs(),
                    std::move(requests)
                );
                if (!created)
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT,
                        created.error().domain,
                        created.error().reason,
                        created.error().message
                    });
                return std::move(*created);
            }
        ));
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{commands::CommandId{"lux.editor.new.scene"}, "New Scene", "File"},
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
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{
                commands::CommandId{"lux.editor.play"},
                "Play Frozen Scene",
                "Scene",
                "Ctrl+P",
                commands::ECommandScope::SESSION
            },
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
            draft.commands.push_back(std::make_shared<commands::CommandEntry>(
                contracts::CodeLease::builtin(),
                commands::CommandDescriptor{
                    commands::CommandId{std::string("lux.editor.scene.") + role},
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
                        auto record = std::ranges::find(content_views_, view, &ContentView::view);
                        if (record == content_views_.end() || !record->run)
                            return cxx::unexpected(
                                commands::CommandFailure{commands::ECommandError::STALE_TARGET, "run.view"}
                            );
                        if (role == "stop")
                        {
                            auto result = stopRun(*record->run);
                            if (!result)
                                return cxx::unexpected(commandFailure(result.error()));
                        }
                        else if (role == "step")
                        {
                            auto owner = std::ranges::find(run_presentations_, record->run, &RunPresentation::run);
                            if (owner == run_presentations_.end() || owner->steps.size() >= 64)
                                return cxx::unexpected(
                                    commands::CommandFailure{commands::ECommandError::BUSY, "run.steps"}
                                );
                            auto step = runs_.step(*record->run);
                            if (!step)
                                return cxx::unexpected(
                                    commandFailure(applicationFailure("run.step", step.error()).value())
                                );
                            owner->steps.push_back(*step);
                        }
                        else
                        {
                            auto result = role == "pause" ? runs_.pause(*record->run) : runs_.resume(*record->run);
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
