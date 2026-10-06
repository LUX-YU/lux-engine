#include <lux/engine/editor/scene/SceneControlInputs.hpp>
#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>

namespace lux::editor::scene
{
    std::span<const SceneProviderOption> defaultSceneProviders() noexcept
    {
        static constexpr SceneProviderOption providers[]{
            {"lux.render.runtime", "main-window"},
            {"lux.render.scene_bindings", "render-bindings"},
            {"lux.render.resources", "resources"},
            {"lux.render.assets", "assets"},
            {"lux.world.loading", "world-storage"}
        };
        return providers;
    }
    namespace
    {
        auto failure(const services::ServiceFailure& error)
        {
            return cxx::unexpected(SceneConfigurationFailure{
                error.code == services::EServiceError::BUSY ? ESceneConfigurationError::BUSY
                                                            : ESceneConfigurationError::CONTROL_FAILURE,
                "configuration.dependencies",
                static_cast<std::uint64_t>(error.code),
                error.detail,
                std::any{error}
            });
        }
    } // namespace
    namespace
    {
        SceneConfigurationInputs assembleInputs(
            const lux::project::PluginCatalog& catalog,
            const SceneConfigurationRegistrations& registrations,
            std::vector<std::shared_ptr<const SceneEditorCatalog::Definition>> definitions,
            std::shared_ptr<const void> foundation_owner
        )
        {
            return SceneConfigurationInputs{
                catalog,
                registrations.components,
                registrations.simulation_systems,
                registrations.scene_systems,
                registrations.features,
                registrations.providers,
                [owner = std::move(foundation_owner), definitions = std::move(definitions)](
                    lux::ui::Element& parent,
                    std::string_view name,
                    std::uint32_t version,
                    const serialization::PortableValueCodec&,
                    std::optional<std::span<const std::byte>> initial
                ) -> SceneConfigurationResult<ConfigurationControl>
                {
                    for (const auto& definition : definitions)
                    {
                        for (const auto& editor : definition->configurations)
                        {
                            const bool is_match =
                                editor.value.schema_name == name && editor.value.schema_version == version;
                            if (is_match)
                            {
                                return makeConfigurationControl(editor, parent, lux::ui::ElementId{name}, initial);
                            }
                        }
                    }
                    // A schema without a contributed control still uses its registered codec default.
                    return ConfigurationControl{};
                },
                registrations.feature_bindings
            };
        }
    } // namespace
    SceneConfigurationResult<SceneConfigurationInputs> makeSceneConfigurationInputs(
        const lux::project::PluginCatalog& catalog,
        const SceneConfigurationRegistrations& registrations,
        std::span<const std::shared_ptr<const services::ServiceEntry>> entries,
        std::shared_ptr<const void> foundation_owner
    )
    {
        auto definitions = sceneEditorDefinitions(entries);
        if (!definitions)
        {
            return failure(definitions.error());
        }
        return assembleInputs(catalog, registrations, std::move(*definitions), std::move(foundation_owner));
    }
    namespace detail
    {
        SceneConfigurationResult<SceneConfigurationInputs> resolveSceneConfigurationInputs(
            services::ServiceResolver& resolver,
            std::size_t offset
        )
        {
            auto catalog = resolver.require<lux::project::PluginManager>(offset);
            if (!catalog)
            {
                return failure(catalog.error());
            }
            auto components = resolver.require<simulation::ecs::ComponentSchemaSet>(offset + 1);
            if (!components)
            {
                return failure(components.error());
            }
            auto simulation = resolver.require<SimulationSystems>(offset + 2);
            if (!simulation)
            {
                return failure(simulation.error());
            }
            if (!simulation->get())
            {
                return failure({services::EServiceError::INVALID_CONFIGURATION, {}, "simulation.systems"});
            }
            auto scene = resolver.require<SceneSystems>(offset + 3);
            if (!scene)
            {
                return failure(scene.error());
            }
            auto features = resolver.require<RenderFeatures>(offset + 4);
            if (!features)
            {
                return failure(features.error());
            }
            auto bindings = resolver.require<RenderBindings>(offset + 5);
            if (!bindings)
            {
                return failure(bindings.error());
            }
            auto definitions = resolver.definitions<SceneEditorCatalog::Definition>(offset + 6);
            if (!definitions)
            {
                return failure(definitions.error());
            }
            return assembleInputs(
                catalog->get().catalog(),
                {components->get(),
                 *simulation->get(),
                 scene->get(),
                 features->get(),
                 defaultSceneProviders(),
                 bindings->get()},
                std::move(*definitions),
                {}
            );
        }
    } // namespace detail
} // namespace lux::editor::scene
