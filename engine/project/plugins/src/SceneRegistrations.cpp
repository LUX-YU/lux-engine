#include <lux/engine/project/SceneRegistrations.hpp>

#include <unordered_set>

namespace lux::project
{
    PluginResult<SceneRegistrations> readSceneRegistrations(
        std::span<const simulation::ecs::ComponentSchema> application_components,
        std::span<const std::shared_ptr<const PluginLibrary>> libraries
    ) noexcept
    {
        SceneRegistrations result;
        std::vector<simulation::ecs::ComponentSchema> schemas(
            application_components.begin(),
            application_components.end()
        );
        auto simulation_systems = std::make_shared<simulation::SimulationSystemRegistry>();
        std::unordered_set<std::uint64_t> scene_types;
        for (const auto& library : libraries)
        {
            if (!library)
            {
                return cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT});
            }
            const auto components = library->components();
            schemas.insert(schemas.end(), components.begin(), components.end());
            if (auto added = simulation_systems->add(library->simulationSystems()); !added)
            {
                return cxx::unexpected(PluginFailure{
                    EPluginError::REGISTRATION_FAILURE,
                    library->identity().id,
                    std::string(added.error().type.name)
                });
            }
            for (const auto& registration : library->sceneSystems())
            {
                if (!scene_types.insert(registration.type.hash).second)
                {
                    return cxx::unexpected(PluginFailure{
                        EPluginError::REGISTRATION_FAILURE,
                        library->identity().id,
                        std::string(registration.type.name)
                    });
                }
                result.scene_systems.push_back(registration);
            }
        }
        auto components = simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
        if (!components)
        {
            return cxx::unexpected(
                PluginFailure{EPluginError::REGISTRATION_FAILURE, {}, std::string(components.error().schema.name)}
            );
        }
        result.components = std::move(*components);
        result.simulation_systems = std::move(simulation_systems);
        return result;
    }
} // namespace lux::project
