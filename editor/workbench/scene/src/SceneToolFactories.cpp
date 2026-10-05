#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/scene/ResourceView.hpp>
#include <lux/engine/editor/scene/RunInspectorView.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/workbench/UiFailure.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::scene
{
    namespace
    {
        using PaneResult = desktop::UiResult<std::unique_ptr<lux::ui::Pane>>;
        template <class Error> auto failure(const Error& error)
        {
            bool is_retryable = workbench::detail::isRetryableUiFailure(error);
            if constexpr (std::same_as<Error, RunFailure>)
            {
                const auto* run = std::get_if<ERunError>(&error.cause);
                is_retryable = is_retryable || (run && *run == ERunError::NOT_READY);
            }
            return cxx::unexpected(workbench::detail::uiFailure(error, is_retryable));
        }
        auto failure(const SceneConfigurationFailure& error)
        {
            const bool is_busy =
                error.code == ESceneConfigurationError::BUSY || workbench::detail::isRetryableUiFailure(error.cause);
            return cxx::unexpected(desktop::UiFailure{
                is_busy ? desktop::EUiError::BUSY : desktop::EUiError::OPERATION_FAILURE,
                error.domain,
                error.reason,
                error.message
            });
        }
        auto failure(const services::ServiceFailure& error)
        {
            return cxx::unexpected(desktop::UiFailure{
                error.code == services::EServiceError::BUSY ? desktop::EUiError::BUSY : desktop::EUiError::DEPENDENCY,
                "services",
                static_cast<std::uint64_t>(error.code),
                error.detail
            });
        }
        desktop::UiResult<void> emptyConfiguration(std::span<const std::byte> bytes) noexcept
        {
            if (!bytes.empty())
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.tool.configuration"}
                );
            }
            return {};
        }
        desktop::UiResult<void> validateInput(const desktop::UiCreateInfo& input)
        {
            // UiRegistry has already checked the descriptor's schema/bytes and common identity.
            const bool is_single =
                input.content.sessions.size() == 1 && input.content.primary == input.content.sessions.front();
            const bool is_invalid = !input.content.valid() || (!input.content.sessions.empty() && !is_single);
            if (is_invalid)
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.tool.content"}
                );
            }
            return {};
        }
        constexpr services::ServiceDependency inspection_dependencies[]{
            {services::ServiceNameView{"lux.editor.sessions"},
             1,
             cxx::typeToken<sessions::SessionStore>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.simulation.components"},
             1,
             cxx::typeToken<simulation::ecs::ComponentSchemaSet>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            // A creation scope lends the existing group owner. The Pane retains this same allocation;
            // neither the resolver nor the lexical scope is kept by the output.
            {services::ServiceNameView{"lux.editor.scene.interaction"},
             1,
             cxx::typeToken<std::shared_ptr<SceneInteractionGroup>>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::SAME,
             {},
             {},
             true},
            {services::ServiceNameView{"lux.editor.scene.runs"},
             1,
             cxx::typeToken<RunStore>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT,
             {},
             {},
             true}
        };
        constexpr services::ServiceDependency inspector_dependencies[]{
            inspection_dependencies[0],
            inspection_dependencies[1],
            inspection_dependencies[2],
            inspection_dependencies[3],
            {services::ServiceNameView{"lux.editor.project.catalog"},
             1,
             cxx::typeToken<project::ProjectCatalogModel>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT,
             {},
             {},
             true},
            {services::ServiceNameView{"lux.editor.scene.inspector.components"},
             1,
             cxx::typeToken<std::vector<InspectorComponent>>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT,
             {},
             {},
             true}
        };
        struct InspectionInput final
        {
            sessions::TSessionAccess<SceneSession> sessions;
            simulation::ecs::ComponentSchemaSet schemas;
            std::shared_ptr<SceneInteractionGroup> group;
            RunStore* runs{};
        };
        desktop::UiResult<InspectionInput> inspectionInput(
            services::ServiceResolver& resolver,
            const desktop::UiCreateInfo& input
        )
        {
            if (auto valid = validateInput(input); !valid)
            {
                return cxx::unexpected(std::move(valid.error()));
            }
            auto store = resolver.require<sessions::SessionStore>(0);
            if (!store)
            {
                return failure(store.error());
            }
            auto schemas = resolver.require<simulation::ecs::ComponentSchemaSet>(1);
            if (!schemas)
            {
                return failure(schemas.error());
            }
            auto selected = resolver.require<std::shared_ptr<SceneInteractionGroup>>(2);
            if (!selected && selected.error().code != services::EServiceError::NOT_FOUND)
            {
                return failure(selected.error());
            }
            auto runs = resolver.require<RunStore>(3);
            if (!runs && runs.error().code != services::EServiceError::NOT_FOUND)
            {
                return failure(runs.error());
            }
            InspectionInput result{
                store->get().access<SceneSession>(),
                schemas->get(),
                selected ? selected->get() : nullptr,
                runs ? &runs->get() : nullptr
            };
            if (result.group)
            {
                const auto author = result.group->session();
                const views::ViewContent association =
                    author ? views::ViewContent{{author->id()}, author->id()} : views::ViewContent{};
                if (association != input.content)
                {
                    return cxx::unexpected(
                        desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.tool.interaction-content"}
                    );
                }
                // This reads through the original Store/Session or Run gate. BUSY must not silently
                // replace a supplied group with a fresh one and erase a pending selection/gesture.
                if (auto synchronized = result.group->synchronize(); !synchronized)
                {
                    return failure(synchronized.error());
                }
            }
            else if (input.content.primary)
            {
                auto key = result.sessions.key(*input.content.primary);
                if (!key)
                {
                    return failure(key.error());
                }
                result.group = std::make_shared<SceneInteractionGroup>(
                    result.sessions,
                    *key,
                    InteractionGroupId{input.instance.hash()},
                    result.runs ? std::optional{result.runs->inspect()} : std::nullopt
                );
                if (auto synchronized = result.group->synchronize(); !synchronized)
                {
                    return failure(synchronized.error());
                }
            }
            return result;
        }
        VSceneViewBinding binding(const std::shared_ptr<SceneInteractionGroup>& group)
        {
            if (group)
            {
                if (const auto author = group->session())
                {
                    return EditedSceneBinding{*author, group.get()};
                }
                if (const auto run = group->run())
                {
                    return RunningSceneBinding{*run, group.get()};
                }
            }
            return UnboundSceneBinding{};
        }
        PaneResult createOutliner(services::ServiceResolver& resolver, const desktop::UiCreateInfo& input)
        {
            auto dependencies = inspectionInput(resolver, input);
            if (!dependencies)
            {
                return cxx::unexpected(std::move(dependencies.error()));
            }
            auto view = std::make_unique<OutlinerView>(
                input.dispatcher,
                input.instance,
                dependencies->sessions,
                binding(dependencies->group),
                dependencies->runs ? std::optional{dependencies->runs->inspect()} : std::nullopt,
                dependencies->schemas,
                dependencies->group
            );
            if (!view->status())
            {
                return failure(view->status().error());
            }
            return std::unique_ptr<lux::ui::Pane>(std::move(view));
        }
        PaneResult createInspector(services::ServiceResolver& resolver, const desktop::UiCreateInfo& input)
        {
            auto dependencies = inspectionInput(resolver, input);
            if (!dependencies)
            {
                return cxx::unexpected(std::move(dependencies.error()));
            }
            if (dependencies->group && dependencies->group->run())
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.inspector.author-required"}
                );
            }
            auto catalog = resolver.require<project::ProjectCatalogModel>(4);
            if (!catalog && catalog.error().code != services::EServiceError::NOT_FOUND)
            {
                return failure(catalog.error());
            }
            auto components = resolver.require<std::vector<InspectorComponent>>(5);
            if (!components && components.error().code != services::EServiceError::NOT_FOUND)
            {
                return failure(components.error());
            }
            auto view = std::make_unique<InspectorView>(
                input.dispatcher,
                input.instance,
                dependencies->sessions,
                dependencies->schemas,
                components ? components->get() : sceneInspectorComponents(),
                catalog ? &catalog->get() : nullptr,
                dependencies->group
            );
            if (!view->status())
            {
                return failure(view->status().error());
            }
            if (dependencies->group)
            {
                const auto& selected = dependencies->group->selection().objects;
                if (!selected.empty())
                {
                    const auto* target = std::get_if<SceneObjectRef>(&selected.front());
                    const auto author = dependencies->group->session();
                    if (!target || !author)
                    {
                        return cxx::unexpected(
                            desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.inspector.target"}
                        );
                    }
                    if (auto bound = view->rebind({*author, dependencies->group.get()}, *target); !bound)
                    {
                        return failure(bound.error());
                    }
                }
            }
            return std::unique_ptr<lux::ui::Pane>(std::move(view));
        }
        PaneResult createRunInspector(services::ServiceResolver& resolver, const desktop::UiCreateInfo& input)
        {
            auto dependencies = inspectionInput(resolver, input);
            if (!dependencies)
            {
                return cxx::unexpected(std::move(dependencies.error()));
            }
            const bool has_author = dependencies->group && dependencies->group->session().has_value();
            const bool is_invalid_binding = !dependencies->runs || has_author || input.content.primary.has_value();
            if (is_invalid_binding)
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.inspector.run-required"}
                );
            }
            auto catalog = resolver.require<project::ProjectCatalogModel>(4);
            if (!catalog && catalog.error().code != services::EServiceError::NOT_FOUND)
            {
                return failure(catalog.error());
            }
            auto view = std::make_unique<RunInspectorView>(
                input.dispatcher,
                input.instance,
                *dependencies->runs,
                dependencies->schemas,
                runInspectorComponents(),
                catalog ? &catalog->get() : nullptr,
                dependencies->group
            );
            if (!view->status())
            {
                return failure(view->status().error());
            }
            if (dependencies->group)
            {
                const auto& selected = dependencies->group->selection().objects;
                if (!selected.empty())
                {
                    const auto* target = std::get_if<RunningObjectRef>(&selected.front());
                    if (!target)
                    {
                        return cxx::unexpected(
                            desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.inspector.run-target"}
                        );
                    }
                    if (auto bound = view->rebind(*target); !bound)
                    {
                        return failure(bound.error());
                    }
                }
            }
            return std::unique_ptr<lux::ui::Pane>(std::move(view));
        }
        constexpr services::ServiceDependency configuration_dependencies[]{
            inspection_dependencies[0],
            {services::ServiceNameView{"lux.editor.scene.configuration"},
             1,
             cxx::typeToken<SceneConfigurationInputs>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        PaneResult createConfiguration(services::ServiceResolver& resolver, const desktop::UiCreateInfo& input)
        {
            if (auto valid = validateInput(input); !valid)
            {
                return cxx::unexpected(std::move(valid.error()));
            }
            auto store = resolver.require<sessions::SessionStore>(0);
            if (!store)
            {
                return failure(store.error());
            }
            auto configuration = resolver.require<SceneConfigurationInputs>(1);
            if (!configuration)
            {
                return failure(configuration.error());
            }
            auto access = store->get().access<SceneSession>();
            auto view = std::make_unique<SceneConfigurationView>(
                input.dispatcher,
                input.instance,
                access,
                configuration->get()
            );
            if (!view->status())
            {
                return failure(view->status().error());
            }
            if (input.content.primary)
            {
                auto key = access.key(*input.content.primary);
                if (!key)
                {
                    return failure(key.error());
                }
                if (auto bound = view->rebind(*key); !bound)
                {
                    return failure(bound.error());
                }
            }
            return std::unique_ptr<lux::ui::Pane>(std::move(view));
        }
        constexpr services::ServiceDependency resource_dependencies[]{
            {services::ServiceNameView{"lux.scene.runtime"},
             1,
             cxx::typeToken<lux::scene::SceneRuntime>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.ui.root"},
             1,
             cxx::typeToken<lux::ui::Root>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::SAME,
             {},
             {},
             true},
            {services::ServiceNameView{"lux.editor.scene.viewport"},
             1,
             cxx::typeToken<lux::ui::PaneHandle>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::SAME,
             {},
             {},
             true}
        };
        PaneResult createResources(services::ServiceResolver& resolver, const desktop::UiCreateInfo& input)
        {
            if (auto valid = validateInput(input); !valid)
            {
                return cxx::unexpected(std::move(valid.error()));
            }
            auto runtime = resolver.require<lux::scene::SceneRuntime>(0);
            if (!runtime)
            {
                return failure(runtime.error());
            }
            auto root = resolver.require<lux::ui::Root>(1);
            if (!root && root.error().code != services::EServiceError::NOT_FOUND)
            {
                return failure(root.error());
            }
            auto source = resolver.require<lux::ui::PaneHandle>(2);
            if (!source && source.error().code != services::EServiceError::NOT_FOUND)
            {
                return failure(source.error());
            }
            if (bool(source) != bool(root))
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.resources.viewport"}
                );
            }
            auto view = std::make_unique<ResourceView>(input.dispatcher, input.instance, runtime->get());
            if (source)
            {
                if (auto followed = view->followViewport(root->get(), source->get()); !followed)
                {
                    return failure(followed.error());
                }
            }
            if (view->content() != input.content)
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.resources.content"}
                );
            }
            return std::unique_ptr<lux::ui::Pane>(std::move(view));
        }
        constexpr sessions::SessionKindIdView scene_kind[]{sessions::SessionKindIdView{"lux.editor.scene"}};
    } // namespace

    constinit const desktop::UiDescriptor kOutlinerView{
        .type = views::ViewTypeIdView{"lux.editor.outliner"},
        .label = "Outliner",
        .dependencies = inspection_dependencies,
        .validate = emptyConfiguration,
        .create = createOutliner,
        .content_kinds = scene_kind,
        .default_content_view = false,
        .content = +[](const lux::ui::Pane& pane) noexcept { return static_cast<const OutlinerView&>(pane).content(); }
    };
    constinit const desktop::UiDescriptor kInspectorView = []
    {
        desktop::UiDescriptor descriptor{
            .type = views::ViewTypeIdView{"lux.editor.inspector"},
            .label = "Inspector",
            .dependencies = inspector_dependencies,
            .validate = emptyConfiguration,
            .create = createInspector,
            .content_kinds = scene_kind,
            .default_content_view = false,
            .content =
                +[](const lux::ui::Pane& pane) noexcept { return static_cast<const InspectorView&>(pane).content(); },
            .prepare_close = +[](lux::ui::Pane& pane) -> desktop::UiResult<void>
            {
                auto result = static_cast<InspectorView&>(pane).prepareClose();
                if (!result)
                {
                    return failure(result.error());
                }
                return {};
            }
        };
        descriptor.cancel_preview = +[](lux::ui::Pane& pane) -> desktop::UiResult<void>
        {
            auto result = static_cast<InspectorView&>(pane).cancelEditing();
            if (!result)
            {
                return failure(result.error());
            }
            return {};
        };
        return descriptor;
    }();
    constinit const desktop::UiDescriptor kRunInspectorView = []
    {
        desktop::UiDescriptor descriptor{
            .type = views::ViewTypeIdView{"lux.editor.run-inspector"},
            .label = "Run Inspector",
            .dependencies = std::span{inspector_dependencies}.first(5),
            .validate = emptyConfiguration,
            .create = createRunInspector,
            .prepare_close = +[](lux::ui::Pane& pane) -> desktop::UiResult<void>
            {
                auto result = static_cast<RunInspectorView&>(pane).prepareClose();
                if (!result)
                {
                    return failure(result.error());
                }
                return {};
            }
        };
        descriptor.cancel_preview = +[](lux::ui::Pane& pane) -> desktop::UiResult<void>
        {
            auto result = static_cast<RunInspectorView&>(pane).cancelEditing();
            if (!result)
            {
                return failure(result.error());
            }
            return {};
        };
        return descriptor;
    }();
    constinit const desktop::UiDescriptor kResourceView{
        .type = views::ViewTypeIdView{"lux.editor.resources"},
        .label = "Resources",
        .dependencies = resource_dependencies,
        .validate = emptyConfiguration,
        .create = createResources,
        .content_kinds = scene_kind,
        .default_content_view = false,
        .content = +[](const lux::ui::Pane& pane) noexcept { return static_cast<const ResourceView&>(pane).content(); }
    };
    constinit const desktop::UiDescriptor kSceneConfigurationView{
        .type = views::ViewTypeIdView{"lux.editor.scene.configuration"},
        .label = "Scene configuration",
        .dependencies = configuration_dependencies,
        .validate = emptyConfiguration,
        .create = createConfiguration,
        .content_kinds = scene_kind,
        .default_content_view = false,
        .content = +[](const lux::ui::Pane& pane) noexcept
                   { return static_cast<const SceneConfigurationView&>(pane).content(); },
        .prepare_close = +[](lux::ui::Pane& pane) -> desktop::UiResult<void>
        {
            auto result = static_cast<SceneConfigurationView&>(pane).prepareClose();
            if (!result)
            {
                return failure(result.error());
            }
            return {};
        }
    };
} // namespace lux::editor::scene
