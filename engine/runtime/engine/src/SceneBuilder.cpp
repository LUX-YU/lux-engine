#include <lux/engine/SceneBuilder.hpp>
#include <lux/engine/EngineMetaFunctionRegistry.hpp>
#include <lux/engine/GeometryDeclaration.hpp>
#include <lux/engine/PartitionDeclaration.hpp>
#include <lux/engine/SceneSystemRegistration.hpp>
#include <lux/engine/SimulationSystemRegistration.hpp>
#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationSystem.hpp>
#include <lux/engine/world/World.hpp>

namespace lux::engine
{
    cxx::expected<std::unique_ptr<scene::SceneInstance>, error::Error>
    Engine::SceneBuilder::build(const scene::SceneDescription& description) const noexcept
    {
        const auto invalid = execution::toError(execution::EExecutionError::INVALID_ARGUMENT);
        for (const auto& geometry : description.world.supported_geometries)
        {
            const auto* declared = metadata_.findGeometry(geometry.name);
            const auto* partitioner = metadata_.findPartitioner(geometry.partitioner.name);
            if (!declared || !partitioner ||
                partitioner->configuration_version != geometry.partitioner.configuration_version)
                return cxx::unexpected(invalid);
        }
        const auto declarations = metadata_.components();
        auto schemas = ecs::ComponentSchemaSet::build(std::vector<ecs::ComponentSchema>{
            declarations.begin(), declarations.end()
        });
        if (!schemas) return cxx::unexpected(invalid);
        auto world = std::make_unique<world::World>(description.world);
        auto simulation = std::make_unique<simulation::Simulation>();
        for (const auto& described : description.simulation.groups)
        {
            if (described.fixed_step && *described.fixed_step <= simulation::SimulationDuration::zero())
                return cxx::unexpected(invalid);
            auto group = described.fixed_step ? simulation::TickGroup{*described.fixed_step} : simulation::TickGroup{};
            for (const auto& system : described.systems)
            {
                const auto* declaration = metadata_.findSimulationSystem(system.type);
                const bool unavailable = !declaration || !declaration->create ||
                    declaration->configuration_version != system.configuration_version;
                if (unavailable) return cxx::unexpected(invalid);
                auto created = declaration->create(context_, *world, system.configuration);
                if (!created) return cxx::unexpected(created.error());
                if (!*created) return cxx::unexpected(invalid);
                group.add(std::move(*created));
            }
            simulation->add(std::move(group));
        }
        auto candidate = std::make_unique<scene::SceneInstance>(std::move(world), std::move(simulation));
        for (const auto& system : description.systems)
        {
            const auto* declaration = metadata_.findSceneSystem(system.type);
            const bool unavailable = !declaration || !declaration->create ||
                declaration->configuration_version != system.configuration_version;
            if (unavailable) return cxx::unexpected(invalid);
            auto created = declaration->create(
                context_, candidate->world(), candidate->simulation(), *schemas, system.configuration
            );
            if (!created) return cxx::unexpected(created.error());
            if (!*created) return cxx::unexpected(invalid);
            candidate->add(std::move(*created));
        }
        return candidate;
    }
}
