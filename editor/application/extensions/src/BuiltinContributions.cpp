#include <lux/engine/editor/extensions/BuiltinContributions.hpp>
#include <algorithm>

namespace lux::editor::extensions
{
    namespace
    {
        commands::CommandFailure commandFailure(const sessions::SessionFactoryFailure& error)
        {
            using enum commands::ECommandError;
            auto code = DOMAIN_FAILURE;
            if (error.code == sessions::ESessionFactoryError::BUSY)
                code = BUSY;
            if (error.code == sessions::ESessionFactoryError::STALE_SESSION)
                code = STALE_TARGET;
            if (error.code == sessions::ESessionFactoryError::STALE_CONTENT)
                code = STALE_CONTENT;
            return {code, error.domain, error.domain_code, error.detail};
        }
        commands::CommandResult<sessions::SessionInfo> targetInfo(
            sessions::SessionStore& store,
            const commands::VCommandTarget& target
        )
        {
            const auto* selected = std::get_if<commands::SessionTarget>(&target);
            if (!selected)
                return cxx::unexpected(
                    commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "session.target"}
                );
            auto info = store.describe(selected->id);
            if (!info)
                return cxx::unexpected(commandFailure(sessions::factoryFailure(info.error())));
            if (info->admission != sessions::EEditAdmission::AVAILABLE)
                return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "session.gate"});
            if (selected->based_on && *selected->based_on != info->current)
                return cxx::unexpected(
                    commands::CommandFailure{commands::ECommandError::STALE_CONTENT, "session.content"}
                );
            return std::move(*info);
        }
        template <class Error> views::ViewFactoryFailure viewFailure(const Error& error)
        {
            if constexpr (std::same_as<Error, views::ViewFactoryFailure>)
                return error;
            else if constexpr (requires { error.index(); })
                return std::visit([](const auto& value) { return viewFailure(value); }, error);
            else if constexpr (requires { error.cause; })
                return viewFailure(error.cause);
            else
            {
                views::ViewFactoryFailure result{
                    views::EViewFactoryError::CONSTRUCT,
                    std::string(cxx::typeToken<Error>().name())
                };
                if constexpr (requires { error.retryable; })
                    if (error.retryable)
                        result.code = views::EViewFactoryError::BUSY;
                if constexpr (requires { error.session; error.code == decltype(error.code)::SESSION; })
                    if (error.code == decltype(error.code)::SESSION)
                        return viewFailure(error.session);
                if constexpr (requires { error == Error::BUSY; })
                    if (error == Error::BUSY)
                        result.code = views::EViewFactoryError::BUSY;
                if constexpr (std::is_enum_v<Error>)
                    result.domain_code = static_cast<std::uint64_t>(error);
                else if constexpr (requires { error.code; })
                    result.domain_code = static_cast<std::uint64_t>(error.code);
                if constexpr (std::is_convertible_v<Error, std::string_view>)
                    result.detail = std::string_view(error);
                else if constexpr (requires { std::string{error.message}; })
                    result.detail = error.message;
                else if constexpr (requires {
                                       error.message.data();
                                       error.message.size();
                                   })
                {
                    const auto end = std::find(error.message.begin(), error.message.end(), '\0');
                    result.detail.assign(error.message.begin(), end);
                }
                return result;
            }
        }
        template <class Input, class Create>
            requires requires(Create& create, const views::ViewFactoryInput& input, const Input& value) {
                create(input, value);
            }
        std::shared_ptr<views::ViewFactoryEntry> viewFactory(
            views::ViewTypeId type,
            std::string label,
            sessions::SessionKindId kind,
            Create create
        )
        {
            return std::make_shared<views::ViewFactoryEntry>(
                contracts::CodeLease::builtin(),
                views::ViewFactoryDescriptor{
                    std::move(type), std::move(label), cxx::typeToken<Input>(), 1, {std::move(kind)}
                },
                [create = std::move(create)](const views::ViewFactoryInput& input
                ) mutable -> views::ViewFactoryResult<views::DetachedView> {
                    auto view = create(input, *static_cast<const Input*>(input.binding()));
                    if (!view)
                        return cxx::unexpected(viewFailure(view.error()));
                    return std::move(*view);
                }
            );
        }
    }
    std::vector<std::shared_ptr<commands::CommandEntry>> builtinSessionCommands(
        SessionActivities activities,
        HistoryActionLookup lookup
    )
    {
        using namespace commands;
        auto& store = activities.sessions;
        auto& saves = activities.saves;
        auto roles = std::make_shared<HistoryActionLookup>(std::move(lookup));
        std::vector<std::shared_ptr<CommandEntry>> entries;
        entries.push_back(std::make_shared<CommandEntry>(
            contracts::CodeLease::builtin(),
            CommandDescriptor{CommandId{"lux.editor.save"}, "Save", "File", "Ctrl+S", ECommandScope::SESSION},
            [&store, roles](const CommandQuery& input) -> CommandResult<CommandState> {
                auto info = targetInfo(store, input.target);
                if (!info)
                    return cxx::unexpected(info.error());
                const bool available = bool(*roles) && (*roles)(info->id);
                return CommandState{available, false, available ? "" : "No save role"};
            },
            [&saves](const CommandInvocation& input) -> CommandResult<DispatchReceipt> {
                // The menu fixes the session; requestSave freezes the content current at actual admission.
                auto admitted = saves.requestSave({std::get<SessionTarget>(input.target()).id});
                if (!admitted)
                    return cxx::unexpected(CommandFailure{
                        admitted.error().code == persistence::EPersistenceError::BUSY ? ECommandError::BUSY
                                                                                      : ECommandError::DOMAIN_FAILURE,
                        "persistence",
                        static_cast<std::uint64_t>(admitted.error().code),
                        admitted.error().detail
                    });
                return DispatchReceipt{AcceptedOperation{"save", admitted->value}};
            }
        ));
        for (bool forward : {false, true})
        {
            entries.push_back(std::make_shared<CommandEntry>(
                contracts::CodeLease::builtin(),
                CommandDescriptor{
                    CommandId{forward ? "lux.editor.redo" : "lux.editor.undo"},
                    forward ? "Redo" : "Undo",
                    "Edit",
                    forward ? "Ctrl+Y" : "Ctrl+Z",
                    ECommandScope::SESSION
                },
                [&store, roles, forward](const CommandQuery& input) -> CommandResult<CommandState> {
                    auto info = targetInfo(store, input.target);
                    if (!info)
                        return cxx::unexpected(info.error());
                    auto* role = (*roles) ? (*roles)(info->id) : nullptr;
                    if (!role)
                        return CommandState{false, false, "No history role"};
                    auto history = role->queryHistory();
                    if (!history)
                        return cxx::unexpected(commandFailure(history.error()));
                    return CommandState{forward ? history->can_redo : history->can_undo};
                },
                [roles, forward](const CommandInvocation& input) -> CommandResult<DispatchReceipt> {
                    auto* role = (*roles) ? (*roles)(std::get<SessionTarget>(input.target()).id) : nullptr;
                    if (!role)
                        return cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "session.role"});
                    auto result = forward ? role->redo() : role->undo();
                    if (!result)
                        return cxx::unexpected(commandFailure(result.error()));
                    return DispatchReceipt{ImmediateCompletion{}};
                }
            ));
        }
        return entries;
    }
    std::vector<std::shared_ptr<sessions::SessionFactoryEntry>> builtinSessionFactories(
        simulation::ecs::ComponentSchemaSet schemas,
        lux::flowforge::FlowSourceEnvironment flow
    )
    {
        return {
            scene::makeSceneSessionFactory(std::move(schemas)),
            material::makeMaterialSessionFactory(),
            flowforge::makeFlowSessionFactory(std::move(flow))
        };
    }
    std::shared_ptr<views::ViewFactoryEntry> builtinSceneViewFactory(scene::SceneViewServices scene)
    {
        return viewFactory<views::ContentViewInput>(
            views::ViewTypeId{"lux.editor.scene.view"}, "Scene", {"lux.editor.scene"},
            [scene](const views::ViewFactoryInput& input, const views::ContentViewInput& value)
                -> views::ViewFactoryResult<views::DetachedView> {
                scene::SceneViewCreateInfo info;
                info.id = input.paneId();
                info.title = value.title.empty() ? "Scene" : value.title;
                info.state.camera.transform.translation = {0, 3, 8};
                auto view = scene::makeSceneView(input.dispatcher(), scene, std::move(info));
                if (!view)
                    return cxx::unexpected(viewFailure(view.error()));
                auto bound = view->rebindContent(value.content);
                if (!bound)
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT, bound.error().domain, bound.error().code, bound.error().message
                    });
                return std::move(*view);
            }
        );
    }
    std::shared_ptr<views::ViewFactoryEntry> builtinMaterialViewFactory(
        sessions::TSessionAccess<material::MaterialSession> sessions,
        lux::scene::SceneRuntime& runtime,
        material::MaterialCompilationService& compilation,
        const scene::ProjectionEnvironment& environment,
        std::span<const render::RenderFeatureRegistration> features,
        project::ProjectCatalogModel* assets
    )
    {
        return viewFactory<views::ContentViewInput>(
            views::ViewTypeId{"lux.editor.material"}, "Material", {"lux.editor.material"},
            [sessions, &runtime, &compilation, &environment, features, assets](
                const views::ViewFactoryInput& input, const views::ContentViewInput& value
            ) {
                return material::makeMaterialContentView(
                    input.dispatcher(), input.paneId(), sessions, runtime, compilation, environment,
                    features, assets, value.content
                );
            }
        );
    }
    std::shared_ptr<views::ViewFactoryEntry> builtinFlowViewFactory(flowforge::FlowViewServices flow)
    {
        return viewFactory<views::ContentViewInput>(
            views::ViewTypeId{"lux.editor.flowforge"}, "FlowForge", {"lux.editor.flowforge"},
            [flow](const views::ViewFactoryInput& input, const views::ContentViewInput& value)
                -> views::ViewFactoryResult<views::DetachedView> {
                auto view = flowforge::makeFlowView(input.dispatcher(), input.paneId(), flow);
                if (!view)
                    return cxx::unexpected(viewFailure(view.error()));
                auto bound = view->rebindContent(value.content);
                if (!bound)
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT, bound.error().domain, bound.error().code, bound.error().message
                    });
                return std::move(*view);
            }
        );
    }
}
