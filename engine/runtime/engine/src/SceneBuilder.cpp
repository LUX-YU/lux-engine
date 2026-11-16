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
    namespace
    {
        template <class Registration, class Description>
        [[nodiscard]] bool canCreate(const Registration* declaration, const Description& system) noexcept
        {
            const bool matches_factory = declaration && declaration->create &&
                declaration->configuration_version == system.configuration_version;
            const bool has_unexpected_payload = system.configuration_version == 0 && !system.configuration.empty();
            return matches_factory && !has_unexpected_payload;
        }
    }

    cxx::expected<std::unique_ptr<scene::SceneInstance>, error::Error>
    Engine::SceneBuilder::build(const scene::SceneDescription& description) const noexcept
    {
        const auto invalid = execution::toError(execution::EExecutionError::INVALID_ARGUMENT);
        for (const auto& geometry : description.world.supported_geometries)
        {
            const auto* declared = metadata_.findGeometry(geometry.name);
            const auto* partitioner = metadata_.findPartitioner(geometry.partitioner.name);
            const bool matches_partitioner = partitioner &&
                partitioner->configuration_version == geometry.partitioner.configuration_version;
            const bool has_unexpected_payload = geometry.partitioner.configuration_version == 0 &&
                !geometry.partitioner.configuration.empty();
            if (!declared || !matches_partitioner || has_unexpected_payload)
            {
                return cxx::unexpected(invalid);
            }
        }
        const auto declarations = metadata_.components();
        auto schemas = ecs::ComponentSchemaSet::build(std::vector<ecs::ComponentSchema>{
            declarations.begin(), declarations.end()
        });
        if (!schemas)
        {
            return cxx::unexpected(invalid);
        }
        auto owned_schemas = std::make_shared<const ecs::ComponentSchemaSet>(std::move(*schemas));
        auto candidate = std::make_unique<scene::SceneInstance>(
            std::make_unique<world::World>(description.world),
            std::make_unique<simulation::Simulation>(),
            owned_schemas
        );
        for (const auto& described : description.simulation.groups)
        {
            if (described.fixed_step && *described.fixed_step <= simulation::SimulationDuration::zero())
            {
                return cxx::unexpected(invalid);
            }
            auto group = described.fixed_step ? simulation::TickGroup{*described.fixed_step} : simulation::TickGroup{};
            for (const auto& system : described.systems)
            {
                const auto* declaration = metadata_.findSimulationSystem(system.type);
                if (!canCreate(declaration, system))
                {
                    return cxx::unexpected(invalid);
                }
                auto created = declaration->create(context_, candidate->world(), system.configuration);
                if (!created)
                {
                    return cxx::unexpected(created.error());
                }
                if (!*created)
                {
                    return cxx::unexpected(invalid);
                }
                group.add(std::move(*created));
            }
            candidate->simulation().add(std::move(group));
        }
        for (const auto& system : description.systems)
        {
            const auto* declaration = metadata_.findSceneSystem(system.type);
            if (!canCreate(declaration, system))
            {
                return cxx::unexpected(invalid);
            }
            auto created = declaration->create(
                context_, candidate->world(), candidate->simulation(), *owned_schemas, system.configuration
            );
            if (!created)
            {
                return cxx::unexpected(created.error());
            }
            if (!*created)
            {
                return cxx::unexpected(invalid);
            }
            candidate->add(std::move(*created));
        }
        return candidate;
    }
}
