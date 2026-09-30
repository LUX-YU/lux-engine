#include <lux/engine/editor/metadata/SceneRegistrations.hpp>
#include <lux/engine/project/PluginLibrary.hpp>
#include <lux/engine/project/PluginRendering.hpp>
namespace lux::editor
{
    lux::project::PluginResult<SceneRegistrations> sceneRegistrations(
        std::span<const lux::simulation::ecs::ComponentSchema> additional,
        std::span<const std::shared_ptr<const lux::project::PluginLibrary>> plugins
    )
    {
        std::vector<lux::simulation::ecs::ComponentSchema> schemas;
        const auto append = [&schemas](auto values) { schemas.insert(schemas.end(), values.begin(), values.end()); };
        append(additional);
        for (const auto& plugin : plugins)
            append(plugin->components());
        auto set = lux::simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
        if (!set)
        {
            return lux::cxx::unexpected(lux::project::PluginFailure{
                lux::project::EPluginError::REGISTRATION_FAILURE,
                {},
                "component.schemas",
                std::to_string(static_cast<std::uint64_t>(set.error().code))
            });
        }
        lux::simulation::SimulationSystemRegistry systems;
        for (const auto& plugin : plugins)
        {
            auto result = systems.add(plugin->simulationSystems());
            if (!result)
                return lux::cxx::unexpected(lux::project::PluginFailure{
                    lux::project::EPluginError::REGISTRATION_FAILURE,
                    plugin->identity().id,
                    "simulation.plugins",
                    std::to_string(static_cast<std::uint64_t>(result.error().code))
                });
        }
        std::vector<lux::scene::SceneSystemRegistration> registrations;
        std::vector<lux::scene::RenderFeatureSceneBinding> loaded_bindings;
        std::vector<lux::render::RenderFeatureRegistration> loaded_features;
        for (const auto& plugin : plugins)
        {
            registrations.insert(registrations.end(), plugin->sceneSystems().begin(), plugin->sceneSystems().end());
            auto graphics = lux::project::readPluginRendering(*plugin, plugins);
            if (!graphics)
                return lux::cxx::unexpected(graphics.error());
            loaded_bindings.insert(loaded_bindings.end(), graphics->bindings.begin(), graphics->bindings.end());
            loaded_features.insert(loaded_features.end(), graphics->features.begin(), graphics->features.end());
        }
        return SceneRegistrations{
            std::move(*set),
            std::make_shared<const lux::simulation::SimulationSystemRegistry>(std::move(systems)),
            std::move(registrations),
            std::move(loaded_features),
            std::move(loaded_bindings)
        };
    }

}
