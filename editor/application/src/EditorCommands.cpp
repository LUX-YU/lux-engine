#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/editor/sessions/SessionCommands.hpp>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>

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
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::CLOSED, "application.phase"});
        auto handle = impl_->commands_.snapshot().find(id.view());
        if (!handle)
            return cxx::unexpected(handle.error());
        return impl_->commands_.execute(std::move(*handle), invocation);
    }
    EditorResult<void> EditorApplication::Impl::installContributions()
    {
        extensions::ContributionDraft draft;
        draft.reflection.push_back({contracts::CodeLease::builtin(), project::registerDesktopSettings});
        draft.settings = builtin_settings_;
        draft.commands = sessions::makeHistoryCommands(sessions_, [this](auto id) { return opening_.find(id); });

        auto exit = commands::CommandEntry::bind<command_lux_editor_exit>(
            contracts::CodeLease::builtin(),
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>
            {
                auto requested = requestExit();
                if (!requested)
                    return cxx::unexpected(
                        commands::CommandFailure{commands::ECommandError::DOMAIN_FAILURE, requested.error().domain}
                    );
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        );
        draft.commands.push_back(std::move(exit));
        installSaveCommands(draft);
        installResultView(draft);
        installWorkspaceView(draft);
        installProjectTools(draft);
        draft.views.push_back(project::makeProjectViewFactory(
            project_->catalogModel(),
            [this](const AssetReference& ref)
            {
                if (phase_ != EApplicationPhase::RUNNING || open_intents_.size() == 64)
                    log::error("application.open", "Asset open intent rejected: application closing or queue full");
                else
                    open_intents_.push_back(ref);
            }
        ));
        draft.views.push_back(tasks::makeTaskViewFactory(task_monitor_));
        const extensions::SessionActivities session_activities{sessions_, saves_};
        const extensions::ProjectActivities project_activities{*project_, writes_, engine_->execution()};
        const extensions::WorkbenchAccess workbench{messages_.dispatcherRef(), desktop_->views(), commands_};
        for (const auto& extension : extensions_)
        {
            auto contributed = extension.contributions();
            if (!contributed)
                return applicationFailure("extension.contribute", contributed.error());
            auto activated = extension.activate({&session_activities, &project_activities, &workbench});
            if (!activated)
                return applicationFailure("extension.activate", activated.error());
            append(contributed->code, activated->code);
            append(contributed->reflection, activated->reflection);
            append(contributed->commands, activated->commands);
            append(contributed->sessions, activated->sessions);
            append(contributed->views, activated->views);
            append(contributed->configurations, activated->configurations);
            append(contributed->components, activated->components);
            append(contributed->settings, activated->settings);
            auto tools = desktop::makeToolCommands(
                contributed->views,
                [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
                { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
                toolOpening()
            );
            if (!tools)
                return applicationFailure("extension.tool-commands", tools.error());
            append(draft.commands, *tools);
            append(draft.code, contributed->code);
            append(draft.reflection, contributed->reflection);
            append(draft.commands, contributed->commands);
            append(draft.sessions, contributed->sessions);
            append(draft.views, contributed->views);
            append(draft.configurations, contributed->configurations);
            append(draft.components, contributed->components);
            append(draft.settings, contributed->settings);
        }
        // Cold startup only: establish the real reflection owners before freezing Flow metadata.
        // No desktop commands or views are exposed until the complete second publication succeeds.
        extensions::ContributionDraft reflection;
        reflection.code = draft.code;
        reflection.reflection = draft.reflection;
        reflection.configurations = draft.configurations;
        auto reflected = extensions::ContributionSnapshot::prepare(std::move(reflection));
        if (!reflected)
            return applicationFailure("reflection.prepare", reflected.error());
        auto accepted = contributions_.enqueue(*reflected);
        if (!accepted)
            return applicationFailure("reflection.enqueue", accepted.error());
        auto registered = contributions_.applyPending();
        if (!registered)
            return applicationFailure("reflection.register", registered.error());
        struct FlowMetadata final
        {
            std::shared_ptr<const void> reflection{acquireEditorReflection()};
            std::vector<const lux::meta::RefClass*> classes;
            std::vector<const lux::meta::RefFunction*> functions;
        };
        auto metadata = std::make_shared<FlowMetadata>();
        const auto& registry = meta::ReflectionRegistry::instance();
        for (const auto& type : registry.classes())
            if (type && type->type.size)
                metadata->classes.push_back(type.get());
        for (const auto& function : registry.functions())
            if (function)
                metadata->functions.push_back(function.get());
        flow_environment_.classes = metadata->classes;
        flow_environment_.functions = metadata->functions;
        flow_environment_.code_lifetime = metadata;
        auto valid = lux::flowforge::validateFlowSourceEnvironment(flow_environment_);
        if (!valid)
            return applicationFailure("flow.metadata", valid.error());
        draft.views.push_back(scene::makeSceneViewFactory(
            sceneServices(),
            [this](const scene::ModelPlacement& value) { receiveModel(value); }
        ));
        draft.views.push_back(material::makeMaterialViewFactory(
            sessions_.access<material::MaterialSession>(),
            engine_->sceneRuntime(),
            material_compilation_,
            environment_,
            registrations_.features,
            &project_->catalogModel(),
            [this](const persistence::DerivedArtifact& value) { receiveArtifact(value); }
        ));
        draft.views.push_back(flowforge::makeFlowViewFactory(
            {sessions_.access<flowforge::FlowSession>(), flow_compilation_, flow_environment_},
            [this](const persistence::DerivedArtifact& value) { receiveArtifact(value); }
        ));
        // Factories capture this established immutable metadata environment, never the uninitialized
        // startup catalog. Their controls and new-content preparations retain the same defining code.
        installContentCommands(draft);
        installSceneCommands(draft);
        draft.sessions.push_back(scene::makeSceneSessionFactory(registrations_.components));
        draft.sessions.push_back(material::makeMaterialSessionFactory());
        draft.sessions.push_back(flowforge::makeFlowSessionFactory(flow_environment_));
        auto prepared = extensions::ContributionSnapshot::prepare(std::move(draft));
        if (!prepared)
            return applicationFailure("contributions.prepare", prepared.error());
        auto queued = contributions_.enqueue(*prepared);
        if (!queued)
            return applicationFailure("contributions.enqueue", queued.error());
        auto installed = contributions_.applyPending();
        if (!installed)
            return applicationFailure("contributions.install", installed.error());
        return {};
    }
    commands::CommandResult<commands::CommandInvocation> EditorApplication::Impl::
        captureCommand(const commands::CommandDescriptor& descriptor, const lux::ui::Pane* pane, const lux::ui::Element*)
    {
        if (descriptor.scope == commands::ECommandScope::APPLICATION)
            return commands::CommandInvocation{};
        if (!pane)
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET, "menu.focus"});
        auto views = desktop_->views().describeAll();
        if (!views)
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "menu.views"});
        const auto focused = std::ranges::find_if(
            *views,
            [&](const auto& view)
            {
                bool same{};
                auto compare = [&](lux::ui::Pane& target) { same = &target == pane; };
                auto visited = desktop_->views().withView(view.id, compare);
                return visited && same;
            }
        );
        if (focused == views->end())
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET, "menu.view"});
        if (descriptor.scope == commands::ECommandScope::VIEW)
            return commands::CommandInvocation{focused->id};
        if (!focused->content.primary)
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET, "menu.content"});
        const auto content = sessions_.describe(*focused->content.primary);
        if (!content)
            return cxx::unexpected(commands::CommandFailure{
                content.error() == sessions::ESessionError::BUSY ? commands::ECommandError::BUSY
                                                                 : commands::ECommandError::STALE_TARGET,
                "menu.session"
            });
        return commands::CommandInvocation{commands::SessionTarget{*focused->content.primary, content->current}};
    }
} // namespace lux::editor::application
