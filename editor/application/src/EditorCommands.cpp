#include <algorithm>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>
#include <lux/engine/editor/sessions/SessionCommands.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/log/Log.hpp>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor
        command_lux_editor_exit{lux::editor::commands::CommandIdView{"lux.editor.exit"}, "Exit", "File", "Alt+X"};
}
namespace lux::editor::application
{
    namespace
    {
        template <class T> void append(std::vector<T>& to, std::vector<T>& from)
        {
            to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
        }
    } // namespace
    commands::CommandResult<commands::DispatchReceipt> EditorApplication::execute(
        commands::CommandId id,
        commands::CommandInvocation invocation
    )
    {
        if (auto ready = impl_->admission(); !ready)
        {
            auto error = std::move(ready.error());
            auto code = commands::ECommandError::DOMAIN_FAILURE;
            switch (error.code)
            {
            case EEditorError::INVALID_STATE:
                // admission checks the owner thread before inspecting dispatch state.
                code = commands::ECommandError::WRONG_THREAD;
                break;
            case EEditorError::BUSY:
                code = commands::ECommandError::BUSY;
                break;
            default:
                break;
            }
            return cxx::unexpected(
                commands::CommandFailure{code, std::move(error.domain), error.reason, std::move(error.message)}
            );
        }
        Impl::Dispatch scope{impl_->dispatching_};
        if (impl_->phase_ != EApplicationPhase::RUNNING)
        {
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::CLOSED, "application.phase"});
        }
        auto handle = impl_->commands_.snapshot().find(id.view());
        if (!handle)
        {
            return cxx::unexpected(handle.error());
        }
        return impl_->commands_.execute(std::move(*handle), invocation);
    }
    EditorResult<void> EditorApplication::Impl::installContributions()
    {
        auto& scope = editor_context_.scope();
        auto components =
            scope.provide(services::ServiceNameView{"lux.simulation.components"}, registrations_.components);
        if (!components)
        {
            return applicationFailure("service.components", components.error());
        }
        auto creation = scope.provide(sessions::kSessionCreation, content_creation_);
        if (!creation)
        {
            return applicationFailure("content.creation", creation.error());
        }
        auto availability = scope.provide(sessions::kSessionCreationAvailability, content_creation_available_);
        if (!availability)
        {
            return applicationFailure("content.creation-availability", availability.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.sessions"}, sessions_); !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.sessions.opening"}, opening_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.persistence.saves"}, saves_); !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.project.storage"}, *project_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.persistence.writes"}, writes_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(
                services::ServiceNameView{"lux.editor.persistence.files"},
                static_cast<persistence::IArtifactStore&>(files_)
            );
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.process.execution"}, engine_->execution());
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.editor.persistence.execution"}, save_execution_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        extensions::ContributionDraft draft;

        draft.reflection.push_back({lux::object::CodeLease::builtin(), project::registerDesktopSettings});
        draft.settings = builtin_settings_;
        draft.commands = sessions::makeHistoryCommands(sessions_, [this](auto id) { return opening_.find(id); });

        auto exit = commands::CommandEntry::bind<command_lux_editor_exit>(
            lux::object::CodeLease::builtin(),
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>
            {
                auto requested = requestExit();
                if (!requested)
                {
                    return cxx::unexpected(
                        commands::CommandFailure{commands::ECommandError::DOMAIN_FAILURE, requested.error().domain}
                    );
                }
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        );
        draft.commands.push_back(std::move(exit));
        installSaveCommands(draft);
        installResultView(draft);
        installWorkspaceView(draft);
        installProjectTools(draft);
        asset_open_ = [this](const AssetReference& ref)
        {
            if (phase_ != EApplicationPhase::RUNNING || open_intents_.size() == 64)
            {
                log::error("application.open", "Asset open intent rejected: application closing or queue full");
            }
            else
            {
                open_intents_.push_back(ref);
            }
        };
        draft.ui.push_back(desktop::UiEntry::bind<tasks::kTaskView>(object::CodeLease::builtin()));
        const extensions::SessionActivities session_activities{sessions_, saves_};
        const extensions::ProjectActivities project_activities{*project_, writes_, engine_->execution()};
        const extensions::WorkbenchAccess workbench{messages_.dispatcherRef(), desktop_->root(), commands_};
        for (const auto& extension : extensions_)
        {
            auto contributed = extension.contributions();
            if (!contributed)
            {
                return applicationFailure("extension.contribute", contributed.error());
            }
            auto activated = extension.activate({&session_activities, &project_activities, &workbench});
            if (!activated)
            {
                return applicationFailure("extension.activate", activated.error());
            }
            append(contributed->code, activated->code);
            append(contributed->reflection, activated->reflection);
            append(contributed->services, activated->services);
            append(contributed->ui, activated->ui);
            append(contributed->commands, activated->commands);
            append(contributed->sessions, activated->sessions);
            append(contributed->settings, activated->settings);
            auto tools = desktop::makeToolCommands(
                contributed->ui,
                [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
                { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
                toolOpening()
            );
            if (!tools)
            {
                return applicationFailure("extension.tool-commands", tools.error());
            }
            append(draft.commands, *tools);
            append(draft.code, contributed->code);
            append(draft.reflection, contributed->reflection);
            append(draft.services, contributed->services);
            append(draft.ui, contributed->ui);
            append(draft.commands, contributed->commands);
            append(draft.sessions, contributed->sessions);
            append(draft.settings, contributed->settings);
        }
        // Cold startup only: establish reflection and service declarations before creating content services.
        // No desktop commands or views are exposed until the complete second publication succeeds.
        extensions::ContributionDraft reflection;
        reflection.code = draft.code;
        reflection.reflection = draft.reflection;
        reflection.services = draft.services;
        auto reflected = extensions::ContributionSnapshot::prepare(std::move(reflection));
        if (!reflected)
        {
            return applicationFailure("reflection.prepare", reflected.error());
        }
        auto accepted = contributions_.enqueue(*reflected);
        if (!accepted)
        {
            return applicationFailure("reflection.enqueue", accepted.error());
        }
        auto registered = contributions_.applyPending();
        if (!registered)
        {
            return applicationFailure("reflection.register", registered.error());
        }
        auto saving = editor_context_.services().get<ProjectContentSaving>(scope);
        if (!saving)
        {
            return applicationFailure("service.content-saving", saving.error());
        }
        content_saving_ = std::move(*saving);
        // Module factories resolve their own compilation/environment owners on demand.
        model_drop_ = [this](const scene::ModelPlacement& value) { receiveModel(value); };
        installContentCommands(draft);
        installSceneCommands(draft);
        auto prepared = extensions::ContributionSnapshot::prepare(std::move(draft));
        if (!prepared)
        {
            return applicationFailure("contributions.prepare", prepared.error());
        }
        auto queued = contributions_.enqueue(*prepared);
        if (!queued)
        {
            return applicationFailure("contributions.enqueue", queued.error());
        }
        auto installed = contributions_.applyPending();
        if (!installed)
        {
            return applicationFailure("contributions.install", installed.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.scene.runtime"}, engine_->sceneRuntime());
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.simulation.systems"}, registrations_.simulation_systems);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.scene.systems"}, registrations_.scene_systems);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.render.scene.bindings"}, registrations_.render_bindings);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.render.runtime"}, engine_->renderContext()->runtime());
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.render.resources"}, engine_->renderContext()->resources());
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.render.features"}, registrations_.features);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.scene.run.inspect"}, run_inspection_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.scene.runs"}, runs_); !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.editor.project.catalog"}, project_->catalogModel());
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.tasks.monitor"}, task_monitor_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.assets.importer"}, *importer_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.project.recent"}, *recent_projects_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.project.plugins"}, plugins_); !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.settings.content"}, settings_content_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.project.open"}, asset_open_); !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.editor.project.import.browse"}, import_browse_request_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.project.recent.open"}, recent_open_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.editor.project.plugins.requests"}, plugin_requests_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.scene.model-drop"}, model_drop_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.results.observe"}, results_observe_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided = scope.provide(services::ServiceNameView{"lux.editor.results.request"}, results_request_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.editor.workspace.observe"}, workspace_observe_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.editor.workspace.request"}, workspace_request_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.editor.project.creation.requests"}, creation_requests_);
            !provided)
        {
            return applicationFailure("service.infrastructure", provided.error());
        }
        scene_configuration_ = std::make_unique<scene::SceneConfigurationInputs>(sceneConfigurationInputs());
        inspector_components_ = scene::sceneInspectorComponents();
        auto definitions = scene::sceneEditorDefinitions(contributions_.snapshot().services());
        if (!definitions)
        {
            return applicationFailure("scene.components", definitions.error());
        }
        for (const auto& definition : *definitions)
        {
            inspector_components_
                .insert(inspector_components_.end(), definition->components.begin(), definition->components.end());
        }
        if (auto provided =
                scope.provide(services::ServiceNameView{"lux.editor.scene.configuration"}, *scene_configuration_);
            !provided)
        {
            return applicationFailure("scene.configuration", provided.error());
        }
        if (auto provided = scope.provide(
                services::ServiceNameView{"lux.editor.scene.inspector.components"},
                inspector_components_
            );
            !provided)
        {
            return applicationFailure("scene.components", provided.error());
        }
        return {};
    }
    commands::CommandResult<commands::CommandInvocation> EditorApplication::Impl::
        captureCommand(const commands::CommandDescriptor& descriptor, const lux::ui::Pane* pane, const lux::ui::Element*)
    {
        if (descriptor.scope == commands::ECommandScope::APPLICATION)
        {
            return commands::CommandInvocation{};
        }
        if (!pane)
        {
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET, "menu.focus"});
        }
        auto target = desktop_->root().identify(*pane);
        if (!target)
        {
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET, "menu.window"});
        }
        if (descriptor.scope == commands::ECommandScope::VIEW)
        {
            return commands::CommandInvocation::forView(*target, object::CodeLease::builtin());
        }
        auto association = editor_context_.ui().content(desktop_->root(), *target);
        if (!association)
        {
            return cxx::unexpected(commands::CommandFailure{
                association.error().code == desktop::EUiError::BUSY ? commands::ECommandError::BUSY
                                                                    : commands::ECommandError::STALE_TARGET,
                "menu.content"
            });
        }
        if (!association->primary)
        {
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET, "menu.content"});
        }
        const auto content = sessions_.describe(*association->primary);
        if (!content)
        {
            return cxx::unexpected(commands::CommandFailure{
                content.error() == sessions::ESessionError::BUSY ? commands::ECommandError::BUSY
                                                                 : commands::ECommandError::STALE_TARGET,
                "menu.session"
            });
        }
        return commands::CommandInvocation{commands::SessionTarget{*association->primary, content->current}};
    }
} // namespace lux::editor::application
