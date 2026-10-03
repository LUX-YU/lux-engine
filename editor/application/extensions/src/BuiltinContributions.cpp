#include <lux/engine/editor/extensions/BuiltinContributions.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/scene/SceneCreationView.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialView.hpp>
#include <lux/engine/editor/flowforge/FlowView.hpp>
#include <algorithm>
#include <random>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_new_material{
        lux::editor::commands::CommandIdView{"lux.editor.new.material"},
        "New Material",
        "File"
    };
    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_new_flow{
        lux::editor::commands::CommandIdView{"lux.editor.new.flow"},
        "New Flow",
        "File"
    };

}
namespace lux::editor::extensions
{
    namespace
    {
        asset::AssetId newAssetId()
        {
            std::mt19937 random{std::random_device{}()};
            return asset::AssetId{uuids::uuid_random_generator{random}()};
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
        template <class View, class Payload>
        views::ViewFactoryResult<void> connectIntent(
            views::DetachedView& view,
            object::TSignal<Payload> View::* signal,
            const std::shared_ptr<cxx::move_only_function<void(const Payload&)>>& receiver
        )
        {
            if (!*receiver)
                return {};
            // The factory just constructed this exact view type; no runtime type probing is needed.
            auto connection = object::LuxObject::connect(
                static_cast<View*>(view.pane()), signal,
                [receiver](const Payload& value) noexcept { (*receiver)(value); }
            );
            if (!connection)
                return cxx::unexpected(viewFailure(connection.error()));
            view.addConnection(std::move(*connection));
            return {};
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
    std::vector<std::shared_ptr<commands::CommandEntry>> builtinContentCommands(
        commands::CommandEntry::Query query, ContentCreation receiver, lux::flowforge::FlowSourceEnvironment environment
    )
    {
        auto available = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        auto create = std::make_shared<ContentCreation>(std::move(receiver));
        const auto state = [available](const commands::CommandQuery& input) { return (*available)(input); };
        return {
            commands::CommandEntry::bind<command_lux_editor_new_material>(
            contracts::CodeLease::builtin(),
                state,
                [create](const commands::CommandInvocation&) {
                    lux::material::MaterialSource source{newAssetId(), "Untitled Material", {}};
                    return (*create)(material::prepareMaterialSession({std::move(source)}, {}, {}));
                }
            ),
            commands::CommandEntry::bind<command_lux_editor_new_flow>(
            contracts::CodeLease::builtin(),
                state,
                [create, environment = std::move(environment)](const commands::CommandInvocation&) {
                    lux::flowforge::FlowSource source;
                    source.id = newAssetId();
                    source.name = "Untitled Flow";
                    return (*create)(flowforge::prepareFlowSession({std::move(source)}, {}, {}, environment));
                }
            )
        };
    }
    std::shared_ptr<views::ViewFactoryEntry> builtinSceneCreationFactory(
        scene::SceneConfigurationInputs configuration, ContentCreation receiver
    )
    {
        auto create = std::make_shared<ContentCreation>(std::move(receiver));
        return std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.scene.creation"}, "New Scene", cxx::typeToken<std::monostate>()
            },
            [configuration = std::move(configuration), create](const views::ViewFactoryInput& input)
                -> views::ViewFactoryResult<views::DetachedView> {
                scene::SceneCreationRequests requests{
                    [schemas = configuration.components, create](const scene::SceneCreationConfiguration& value)
                        -> scene::SceneConfigurationResult<void> {
                        auto package = lux::scene::createScenePackage(
                            newAssetId(), value.name, value.schemas, value.simulation, value.scene
                        );
                        if (!package)
                            return cxx::unexpected(scene::SceneConfigurationFailure{
                                scene::ESceneConfigurationError::CONTROL_FAILURE, "scene.creation.package",
                                0, {}, std::any{package.error()}
                            });
                        auto installed = (*create)(scene::prepareSceneSession({std::move(*package)}, {}, {}, schemas));
                        if (!installed)
                        {
                            const auto& error = installed.error();
                            const auto code = error.code == commands::ECommandError::BUSY
                                ? scene::ESceneConfigurationError::BUSY : scene::ESceneConfigurationError::CONTROL_FAILURE;
                            return cxx::unexpected(scene::SceneConfigurationFailure{
                                code, error.domain, error.domain_code, error.detail, std::any{error}
                            });
                        }
                        return {};
                    }
                };
                auto view = scene::makeSceneCreationView(
                    input.dispatcher(), input.paneId(), configuration, std::move(requests)
                );
                if (!view)
                    return cxx::unexpected(viewFailure(view.error()));
                return std::move(*view);
            }
        );
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
    std::shared_ptr<views::ViewFactoryEntry> builtinSceneViewFactory(scene::SceneViewServices scene, ModelIntent receiver)
    {
        auto intent = std::make_shared<ModelIntent>(std::move(receiver));
        return viewFactory<views::ContentViewInput>(
            views::ViewTypeId{"lux.editor.scene.view"}, "Scene", {"lux.editor.scene"},
            [scene, intent](const views::ViewFactoryInput& input, const views::ContentViewInput& value)
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
                auto connected = connectIntent(*view, &scene::SceneView::modelDropped, intent);
                if (!connected)
                    return cxx::unexpected(std::move(connected.error()));
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
        project::ProjectCatalogModel* assets,
        ArtifactIntent receiver
    )
    {
        auto intent = std::make_shared<ArtifactIntent>(std::move(receiver));
        return viewFactory<views::ContentViewInput>(
            views::ViewTypeId{"lux.editor.material"}, "Material", {"lux.editor.material"},
            [sessions, &runtime, &compilation, &environment, features, assets, intent](
                const views::ViewFactoryInput& input, const views::ContentViewInput& value
            ) -> views::ViewFactoryResult<views::DetachedView> {
                auto view = material::makeMaterialContentView(
                    input.dispatcher(), input.paneId(), sessions, runtime, compilation, environment,
                    features, assets, value.content
                );
                if (!view)
                    return cxx::unexpected(viewFailure(view.error()));
                auto connected = connectIntent(*view, &material::MaterialView::publishRequested, intent);
                if (!connected)
                    return cxx::unexpected(std::move(connected.error()));
                return std::move(*view);
            }
        );
    }
    std::shared_ptr<views::ViewFactoryEntry> builtinFlowViewFactory(flowforge::FlowViewServices flow, ArtifactIntent receiver)
    {
        auto intent = std::make_shared<ArtifactIntent>(std::move(receiver));
        return viewFactory<views::ContentViewInput>(
            views::ViewTypeId{"lux.editor.flowforge"}, "FlowForge", {"lux.editor.flowforge"},
            [flow, intent](const views::ViewFactoryInput& input, const views::ContentViewInput& value)
                -> views::ViewFactoryResult<views::DetachedView> {
                auto view = flowforge::makeFlowView(input.dispatcher(), input.paneId(), flow);
                if (!view)
                    return cxx::unexpected(viewFailure(view.error()));
                auto bound = view->rebindContent(value.content);
                if (!bound)
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT, bound.error().domain, bound.error().code, bound.error().message
                    });
                auto connected = connectIntent(*view, &flowforge::FlowView::publishRequested, intent);
                if (!connected)
                    return cxx::unexpected(std::move(connected.error()));
                return std::move(*view);
            }
        );
    }
}
