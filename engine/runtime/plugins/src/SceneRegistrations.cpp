#include <lux/engine/plugins/SceneRegistrations.hpp>

#include <algorithm>
#include <set>
#include <unordered_set>

namespace lux::plugins
{
    PluginResult<SceneRegistrations> readSceneRegistrations(
        std::span<const ecs::ComponentSchema> application_components,
        std::span<const std::shared_ptr<const PluginLibrary>> libraries,
        const scene::SceneContentAllowance* allowed
    ) noexcept
    {
        SceneRegistrations result;
        std::vector<ecs::ComponentSchema> schemas(
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
            result.geometries
                .insert(result.geometries.end(), library->geometries().begin(), library->geometries().end());
            result.partitions
                .insert(result.partitions.end(), library->partitions().begin(), library->partitions().end());
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
        auto components = ecs::ComponentSchemaSet::build(std::move(schemas));
        if (!components)
        {
            return cxx::unexpected(
                PluginFailure{EPluginError::REGISTRATION_FAILURE, {}, std::string(components.error().schema.name)}
            );
        }
        result.components = std::move(*components);
        result.simulation_systems = std::move(simulation_systems);
        std::set<std::string_view> geometry_names, partition_names;
        for (const auto& declaration : result.geometries)
        {
            if (!scene::validGeometryDeclaration(declaration) || !geometry_names.insert(declaration.name).second)
            {
                return cxx::unexpected(PluginFailure{EPluginError::REGISTRATION_FAILURE, {}, declaration.name});
            }
            for (const auto& component : declaration.components)
            {
                const auto* registered = result.components.find(component.schema);
                if (!registered || registered->version != component.version || registered->cpp_type != component.type)
                {
                    return cxx::unexpected(PluginFailure{EPluginError::REGISTRATION_FAILURE, {}, component.schema.name}
                    );
                }
            }
            result.allowed_content.geometries.push_back({declaration.name, declaration.version});
        }
        for (const auto& declaration : result.partitions)
        {
            if (!world::validPartitionDeclaration(declaration) ||
                !partition_names.insert(declaration.identity.id.name).second)
            {
                return cxx::unexpected(
                    PluginFailure{EPluginError::REGISTRATION_FAILURE, {}, declaration.identity.id.name}
                );
            }
            result.allowed_content.partitions.push_back({declaration.identity.id.name, declaration.identity.version});
        }
        if (allowed)
        {
            const auto valid_subset = [](const auto& subset, const auto& available)
            {
                std::set<std::string_view> names;
                for (const auto& choice : subset)
                {
                    if (!names.insert(choice.name).second || std::ranges::find(available, choice) == available.end())
                    {
                        return false;
                    }
                }
                return true;
            };
            if (!valid_subset(allowed->geometries, result.allowed_content.geometries) ||
                !valid_subset(allowed->partitions, result.allowed_content.partitions))
            {
                return cxx::unexpected(PluginFailure{EPluginError::REGISTRATION_FAILURE, {}, "project.content"});
            }
            result.allowed_content = *allowed;
        }
        return result;
    }
} // namespace lux::plugins
