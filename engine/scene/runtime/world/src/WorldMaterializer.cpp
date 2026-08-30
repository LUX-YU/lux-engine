#include <lux/engine/scene/WorldMaterializer.hpp>

#include <new>
#include <utility>

namespace lux::scene
{
    namespace
    {
        [[nodiscard]] WorldMaterializeFailure materializeFailure(
            EWorldMaterializeError code,
            std::size_t object = 0U,
            std::size_t data = 0U
        ) noexcept
        {
            return WorldMaterializeFailure{code, {}, object, data};
        }
    }

    WorldMaterializer::WorldMaterializer(
        std::shared_ptr<const world::WorldDescription> world,
        simulation::ecs::ComponentSchemaSet components,
        std::vector<const simulation::ecs::ComponentSchema*> mappings
    ) noexcept
        : world_(std::move(world)), components_(std::move(components)), mappings_(std::move(mappings))
    {
    }

    lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure> WorldMaterializer::create(
        std::shared_ptr<const world::WorldDescription> world,
        simulation::ecs::ComponentSchemaSet components
    ) noexcept
    {
        if (!world)
        {
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA)
            );
        }

        try
        {
            std::vector<const simulation::ecs::ComponentSchema*> mappings;
            mappings.reserve(world->schemas().size());
            for (const auto& schema : world->schemas())
            {
                mappings.push_back(
                    components.find(simulation::ecs::componentSchemaId(schema.name))
                );
            }
            return WorldMaterializer(std::move(world), std::move(components), std::move(mappings));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::ALLOCATION_FAILURE)
            );
        }
    }

    lux::cxx::expected<simulation::ecs::Entity, WorldMaterializeFailure> WorldMaterializer::object(
        simulation::ecs::Registry& registry,
        world::WorldPartitionObjectView object
    ) const noexcept
    {
        if (!object)
        {
            return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_OBJECT));
        }
        if (object.bundle() != world_->bundleId() || object.generation() != world_->generation())
        {
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA)
            );
        }

        simulation::ecs::Entity entity{simulation::ecs::NullEntity};
        try
        {
            entity = registry.create();
            for (std::size_t data{}; data < object.dataCount(); ++data)
            {
                const std::uint32_t ordinal = object.schemaOrdinalAt(data);
                if (ordinal >= mappings_.size())
                {
                    registry.destroy(entity);
                    return lux::cxx::unexpected(
                        materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA, 0U, data)
                    );
                }
                const auto* schema = mappings_[ordinal];
                if (schema == nullptr)
                {
                    continue;
                }
                if (schema->decode_emplace == nullptr)
                {
                    registry.destroy(entity);
                    return lux::cxx::unexpected(
                        materializeFailure(EWorldMaterializeError::COMPONENT_DECODE_FAILURE, 0U, data)
                    );
                }
                auto decoded = schema->decode_emplace(
                    registry,
                    entity,
                    object.schemaVersionAt(data),
                    object.payloadAt(data)
                );
                if (!decoded)
                {
                    registry.destroy(entity);
                    WorldMaterializeFailure failure =
                        materializeFailure(EWorldMaterializeError::COMPONENT_DECODE_FAILURE, 0U, data);
                    failure.component = decoded.error();
                    return lux::cxx::unexpected(std::move(failure));
                }
            }
            return entity;
        }
        catch (const std::bad_alloc&)
        {
            if (registry.valid(entity))
            {
                registry.destroy(entity);
            }
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::ALLOCATION_FAILURE)
            );
        }
    }

    lux::cxx::expected<void, WorldMaterializeFailure> WorldMaterializer::partition(
        simulation::ecs::Registry& registry,
        const world::WorldPartitionData& data,
        std::vector<simulation::ecs::Entity>* created
    ) const noexcept
    {
        if (data.bundle() != world_->bundleId() || data.generation() != world_->generation())
        {
            if (created != nullptr)
            {
                created->clear();
            }
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA)
            );
        }
        std::vector<simulation::ecs::Entity> local;
        try
        {
            local.reserve(data.objectCount());
            for (std::size_t object_index{}; object_index < data.objectCount(); ++object_index)
            {
                auto entity = object(registry, data.objectAt(object_index));
                if (!entity)
                {
                    for (const auto value : local)
                    {
                        if (registry.valid(value))
                        {
                            registry.destroy(value);
                        }
                    }
                    if (created != nullptr)
                    {
                        created->clear();
                    }
                    auto failure = entity.error();
                    failure.object = object_index;
                    return lux::cxx::unexpected(std::move(failure));
                }
                local.push_back(*entity);
            }
            if (created != nullptr)
            {
                *created = std::move(local);
            }
            return {};
        }
        catch (const std::bad_alloc&)
        {
            for (const auto value : local)
            {
                if (registry.valid(value))
                {
                    registry.destroy(value);
                }
            }
            if (created != nullptr)
            {
                created->clear();
            }
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::ALLOCATION_FAILURE)
            );
        }
    }
}
