#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>

namespace lux::editor::application
{
    namespace
    {
        template <class T> void append(std::vector<T>& to, std::vector<T>& from)
        {
            to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
        }
        template <class Error> views::ViewFactoryFailure constructionFailure(std::string domain, const Error& error)
        {
            if constexpr (requires { error.index(); })
                return std::visit([&](const auto& value) { return constructionFailure(domain, value); }, error);
            else if constexpr (std::is_enum_v<Error>)
                return {views::EViewFactoryError::CONSTRUCT, std::move(domain), static_cast<std::uint64_t>(error)};
            else if constexpr (requires { error.code; })
                return {views::EViewFactoryError::CONSTRUCT, std::move(domain), static_cast<std::uint64_t>(error.code)};
            else
                return {views::EViewFactoryError::CONSTRUCT, std::move(domain)};
        }
    }
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
        auto result = impl_->commands_.execute(std::move(*handle), invocation);
        if (result)
            if (const auto* operation = std::get_if<commands::AcceptedOperation>(&*result);
                operation && operation->kind == "save")
                if (std::ranges::find(impl_->pending_saves_, persistence::SaveId{operation->value}) ==
                    impl_->pending_saves_.end())
                    impl_->pending_saves_.push_back({operation->value});
        return result;
    }
    EditorResult<void> EditorApplication::Impl::installContributions()
    {
        extensions::ContributionDraft draft;
        draft.commands =
            extensions::builtinSessionCommands({sessions_, saves_}, [this](auto id) { return opening_.find(id); });

        draft.views.push_back(extensions::builtinSceneViewFactory(sceneServices()));
        draft.views.push_back(extensions::builtinMaterialViewFactory(
            sessions_.access<material::MaterialSession>(), engine_->sceneRuntime(), material_compilation_,
            environment_, registrations_.features, &project_->catalogModel()
        ));
        draft.views.push_back(extensions::builtinFlowViewFactory(
            {sessions_.access<flowforge::FlowSession>(), flow_compilation_, flow_environment_}
        ));
        auto exit = std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{commands::CommandId{"lux.editor.exit"}, "Exit", "File", "Alt+X"},
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto requested = requestExit();
                if (!requested)
                    return cxx::unexpected(
                        commands::CommandFailure{commands::ECommandError::DOMAIN_FAILURE, requested.error().domain}
                    );
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        );
        draft.commands.push_back(std::move(exit));
        std::erase_if(draft.commands, [](const auto& entry) {
            return entry->descriptor().id == commands::CommandId{"lux.editor.save"};
        });
        installContentCommands(draft);
        installSaveCommands(draft);
        installResultView(draft);
        installWorkspaceView(draft);
        installProjectTools(draft);
        installSceneCommands(draft);
        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.project"},
                "Assets",
                cxx::typeToken<std::monostate>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                return makeProjectView(input.paneId());
            }
        ));
        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.tasks"},
                "Tasks",
                cxx::typeToken<std::monostate>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                return tasks::makeTaskView(input.dispatcher(), input.paneId(), task_monitor_);
            }
        ));
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
            for (const auto& entry : contributed->views)
            {
                const auto& descriptor = entry->descriptor();
                if (descriptor.binding_type != cxx::typeToken<std::monostate>())
                    continue;
                draft.commands.push_back(std::make_shared<commands::CommandEntry>(
                    contracts::CodeLease::builtin(),
                    commands::CommandDescriptor{
                        commands::CommandId{std::string("lux.editor.tool/") + std::string(descriptor.type.name())},
                        descriptor.label,
                        "Window"
                    },
                    [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                        return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
                    },
                    [this, type = descriptor.type](const commands::CommandInvocation&)
                        -> commands::CommandResult<commands::DispatchReceipt> {
                        auto shown = showTool(type);
                        if (!shown)
                            return cxx::unexpected(commands::CommandFailure{
                                commands::ECommandError::DOMAIN_FAILURE,
                                shown.error().domain,
                                shown.error().reason,
                                shown.error().message
                            });
                        return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                    }
                ));
            }
            append(draft.code, contributed->code);
            append(draft.reflection, contributed->reflection);
            append(draft.commands, contributed->commands);
            append(draft.sessions, contributed->sessions);
            append(draft.views, contributed->views);
            append(draft.configurations, contributed->configurations);
            append(draft.components, contributed->components);
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
        auto builtins = extensions::builtinSessionFactories(registrations_.components, flow_environment_);
        append(draft.sessions, builtins);
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
        const auto focused = std::ranges::find_if(*views, [&](const auto& view) {
            bool same{};
            auto compare = [&](lux::ui::Pane& target) { same = &target == pane; };
            auto visited = desktop_->views().withView(view.id, compare);
            return visited && same;
        });
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
}
