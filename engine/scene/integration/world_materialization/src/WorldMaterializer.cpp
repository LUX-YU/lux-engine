#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <limits>
#include <utility>

namespace lux::scene
{
    namespace
    {
        [[nodiscard]] WorldMaterializeFailure materializeFailure(
            EWorldMaterializeError code, std::size_t object = 0, std::size_t data = 0) noexcept
        { return {code, {}, object, data}; }
    }

    WorldMaterializer::WorldMaterializer(std::shared_ptr<const world::WorldDescription> world,
        simulation::ecs::ComponentSchemaSet components,
        std::vector<const simulation::ecs::ComponentSchema*> mappings) noexcept
        : world_(std::move(world)), components_(std::move(components)), mappings_(std::move(mappings)) {}

    lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure> WorldMaterializer::create(
        std::shared_ptr<const world::WorldDescription> world, simulation::ecs::ComponentSchemaSet components) noexcept
    {
        if (!world) return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA));
        std::vector<const simulation::ecs::ComponentSchema*> mappings;
        mappings.reserve(world->schemas().size());
        for (const auto& schema : world->schemas())
            mappings.push_back(components.find(simulation::ecs::componentSchemaId(schema.name)));
        return WorldMaterializer(std::move(world), std::move(components), std::move(mappings));
    }

    lux::cxx::expected<void, WorldMaterializeFailure> WorldMaterializer::decode(
        simulation::ecs::Registry& registry, const simulation::ecs::WorldEntityMap& identities,
        simulation::ecs::Entity entity, world::WorldPartitionObjectView object) const noexcept
    {
        for (std::size_t data{}; data < object.dataCount(); ++data)
        {
            const auto ordinal = object.schemaOrdinalAt(data);
            if (ordinal >= mappings_.size())
                return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA, 0, data));
            const auto* schema = mappings_[ordinal];
            if (!schema) continue;
            if (!schema->decode_emplace)
            {
                auto failure = materializeFailure(EWorldMaterializeError::COMPONENT_DECODE_FAILURE, 0, data);
                failure.component.code = simulation::ecs::EComponentDecodeError::UNSUPPORTED_TYPE;
                return lux::cxx::unexpected(std::move(failure));
            }
            auto decoded = schema->decode_emplace(
                registry, identities, entity, object.schemaVersionAt(data), object.payloadAt(data));
            if (!decoded)
            {
                auto failure = materializeFailure(EWorldMaterializeError::COMPONENT_DECODE_FAILURE, 0, data);
                failure.component = decoded.error();
                return lux::cxx::unexpected(std::move(failure));
            }
        }
        return {};
    }

    lux::cxx::expected<simulation::ecs::Entity, WorldMaterializeFailure> WorldMaterializer::object(
        simulation::ecs::Registry& registry, simulation::ecs::WorldEntityMap& identities,
        world::WorldPartitionObjectView object) const noexcept
    {
        if (!object) return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_OBJECT));
        if (object.bundle() != world_->bundleId() || object.generation() != world_->generation())
            return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA));
        if (identities.entity(object.id()) != simulation::ecs::NullEntity)
            return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::DUPLICATE_OBJECT));
        const auto entity = registry.create();
        const bool bound = identities.bind(object.id(), entity);
        lux::cxx::scope_exit rollback([&]() noexcept {
            if (bound) identities.unbind(entity);
            if (registry.valid(entity)) registry.destroy(entity);
        });
        if (!bound) return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::DUPLICATE_OBJECT));
        auto decoded = decode(registry, identities, entity, object);
        if (!decoded) return lux::cxx::unexpected(std::move(decoded.error()));
        rollback.release();
        return entity;
    }

    lux::cxx::expected<void, WorldMaterializeFailure> WorldMaterializer::partition(
        simulation::ecs::Registry& registry, simulation::ecs::WorldEntityMap& identities,
        const world::WorldPartitionData& data, std::vector<simulation::ecs::Entity>* created) const noexcept
    {
        if (data.bundle() != world_->bundleId() || data.generation() != world_->generation())
            return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA));
        std::vector<world::WorldPartitionObjectView> input;
        input.reserve(data.objectCount());
        for (std::size_t index{}; index < data.objectCount(); ++index) input.push_back(data.objectAt(index));
        return objects(registry, identities, input, created);
    }

    lux::cxx::expected<void, WorldMaterializeFailure> WorldMaterializer::objects(
        simulation::ecs::Registry& registry, simulation::ecs::WorldEntityMap& identities,
        std::span<const world::WorldPartitionObjectView> input,
        std::vector<simulation::ecs::Entity>* created) const noexcept
    {
        for (std::size_t index{}; index < input.size(); ++index)
        {
            const auto object = input[index];
            if (!object) return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_OBJECT, index));
            if (object.bundle() != world_->bundleId() || object.generation() != world_->generation())
                return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA, index));
        }
        for (std::size_t index{}; index < input.size(); ++index)
            if (identities.entity(input[index].id()) != simulation::ecs::NullEntity)
                return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::DUPLICATE_OBJECT, index));
        if (input.size() > (std::numeric_limits<std::size_t>::max)() - identities.size())
            return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_OBJECT));
        std::vector<simulation::ecs::Entity> local;
        local.reserve(input.size());
        identities.reserve(identities.size() + input.size());
        lux::cxx::scope_exit rollback([&]() noexcept {
            for (const auto entity : local)
            {
                identities.unbind(entity);
                if (registry.valid(entity)) registry.destroy(entity);
            }
        });
        // One identity pass for the whole batch, regardless of physical partition order.
        for (std::size_t index{}; index < input.size(); ++index)
        {
            const auto entity = registry.create();
            if (!identities.bind(input[index].id(), entity))
            {
                registry.destroy(entity);
                return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::DUPLICATE_OBJECT, index));
            }
            local.push_back(entity);
        }
        for (std::size_t index{}; index < input.size(); ++index)
        {
            auto decoded = decode(registry, identities, local[index], input[index]);
            if (!decoded)
            {
                auto failure = decoded.error();
                failure.object = index;
                return lux::cxx::unexpected(std::move(failure));
            }
        }
        if (created) *created = std::move(local);
        rollback.release();
        return {};
    }
}
