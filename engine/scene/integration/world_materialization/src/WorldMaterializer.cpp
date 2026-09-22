#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/simulation/ecs/EntityCreationPlan.hpp>

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace lux::scene
{
namespace
{
namespace ecs = simulation::ecs;

WorldMaterializeFailure failure(EWorldMaterializeError code, std::size_t object = 0, std::size_t data = 0)
{
    return {code, {}, object, data};
}

struct PreparedValue final
{
    ecs::Entity entity;
    ecs::DecodedComponent value;
};

struct BatchIdentities final
{
    const ecs::Registry &registry;
    const ecs::WorldEntityMap &resident;
    std::unordered_map<world::WorldObjectId, ecs::Entity, world::WorldObjectIdHash> incoming;

    static lux::cxx::expected<ecs::Entity, ecs::ComponentDecodeFailure> resolve(const void *state,
                                                                                world::WorldObjectId id) noexcept
    {
        const auto &self = *static_cast<const BatchIdentities *>(state);
        if (!id.valid())
        {
            return ecs::NullEntity;
        }
        if (const auto found = self.incoming.find(id); found != self.incoming.end())
        {
            return found->second;
        }
        const auto entity = self.resident.entity(id);
        if (entity != ecs::NullEntity && self.registry.valid(entity))
        {
            return entity;
        }
        return lux::cxx::unexpected(
            ecs::ComponentDecodeFailure{ecs::EComponentDecodeError::UNRESOLVED_REFERENCE, 0, id});
    }
};
} // namespace

WorldMaterializer::WorldMaterializer(std::shared_ptr<const world::WorldDescription> world,
                                     ecs::ComponentSchemaSet components,
                                     std::vector<const ecs::ComponentSchema *> mappings) noexcept
    : world_(std::move(world)), components_(std::move(components)), mappings_(std::move(mappings))
{
}

lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure> WorldMaterializer::create(
    std::shared_ptr<const world::WorldDescription> world, ecs::ComponentSchemaSet components) noexcept
{
    if (!world)
    {
        return lux::cxx::unexpected(failure(EWorldMaterializeError::INVALID_WORLD_SCHEMA));
    }
    std::vector<const ecs::ComponentSchema *> mappings;
    mappings.reserve(world->schemas().size());
    for (const auto &schema : world->schemas())
    {
        mappings.push_back(components.find(ecs::componentSchemaId(schema.name)));
    }
    return WorldMaterializer(std::move(world), std::move(components), std::move(mappings));
}

lux::cxx::expected<ecs::Entity, WorldMaterializeFailure> WorldMaterializer::object(
    ecs::Registry &registry, ecs::WorldEntityMap &identities, world::WorldPartitionObjectView input) const noexcept
{
    std::vector<ecs::Entity> created;
    auto result = objects(registry, identities, std::span(&input, 1), &created);
    if (!result)
    {
        return lux::cxx::unexpected(result.error());
    }
    return created.front();
}

lux::cxx::expected<void, WorldMaterializeFailure> WorldMaterializer::partition(
    ecs::Registry &registry, ecs::WorldEntityMap &identities, const world::WorldPartitionData &data,
    std::vector<ecs::Entity> *created) const noexcept
{
    if (data.bundle() != world_->bundleId() || data.generation() != world_->generation())
    {
        return lux::cxx::unexpected(failure(EWorldMaterializeError::INVALID_WORLD_SCHEMA));
    }
    std::vector<world::WorldPartitionObjectView> input;
    input.reserve(data.objectCount());
    for (std::size_t index{}; index < data.objectCount(); ++index)
    {
        input.push_back(data.objectAt(index));
    }
    return objects(registry, identities, input, created);
}

lux::cxx::expected<void, WorldMaterializeFailure> WorldMaterializer::objects(
    ecs::Registry &registry, ecs::WorldEntityMap &identities, std::span<const world::WorldPartitionObjectView> input,
    std::vector<ecs::Entity> *created, std::size_t maximum_component_bytes, std::size_t *component_bytes) const noexcept
{
    if (input.size() > (std::numeric_limits<std::size_t>::max)() - identities.size())
    {
        return lux::cxx::unexpected(failure(EWorldMaterializeError::CAPACITY));
    }
    auto planned = ecs::planEntityCreation(registry, input.size());
    if (!planned)
    {
        return lux::cxx::unexpected(failure(EWorldMaterializeError::CAPACITY));
    }
    BatchIdentities batch{registry, identities};
    batch.incoming.reserve(input.size());
    for (std::size_t index{}; index < input.size(); ++index)
    {
        const auto object = input[index];
        if (!object || !object.id().valid())
        {
            return lux::cxx::unexpected(failure(EWorldMaterializeError::INVALID_OBJECT, index));
        }
        if (object.bundle() != world_->bundleId() || object.generation() != world_->generation())
        {
            return lux::cxx::unexpected(failure(EWorldMaterializeError::INVALID_WORLD_SCHEMA, index));
        }
        if (identities.entity(object.id()) != ecs::NullEntity ||
            !batch.incoming.emplace(object.id(), planned->entities()[index]).second)
        {
            return lux::cxx::unexpected(failure(EWorldMaterializeError::DUPLICATE_OBJECT, index));
        }
    }

    std::vector<PreparedValue> values;
    std::size_t accounted{};
    const ecs::ComponentEntityResolver resolver{&batch, &BatchIdentities::resolve};
    for (std::size_t index{}; index < input.size(); ++index)
    {
        const auto object = input[index];
        std::vector<lux::cxx::TypeToken> types;
        types.reserve(object.dataCount());
        for (std::size_t data{}; data < object.dataCount(); ++data)
        {
            const auto ordinal = object.schemaOrdinalAt(data);
            if (ordinal >= mappings_.size())
            {
                return lux::cxx::unexpected(failure(EWorldMaterializeError::INVALID_WORLD_SCHEMA, index, data));
            }
            const auto *schema = mappings_[ordinal];
            if (!schema)
            {
                continue; // The source owner retains unknown payloads.
            }
            if (!schema->decode_value || std::ranges::find(types, schema->cpp_type) != types.end())
            {
                auto error = failure(EWorldMaterializeError::COMPONENT_DECODE_FAILURE, index, data);
                error.component.code = ecs::EComponentDecodeError::UNSUPPORTED_TYPE;
                return lux::cxx::unexpected(std::move(error));
            }
            types.push_back(schema->cpp_type);
            auto value = schema->decode_value(object.schemaVersionAt(data), object.payloadAt(data), resolver,
                                              schema->code_lifetime);
            if (!value)
            {
                auto error = failure(EWorldMaterializeError::COMPONENT_DECODE_FAILURE, index, data);
                error.component = value.error();
                return lux::cxx::unexpected(std::move(error));
            }
            if (value->accountedBytes() > maximum_component_bytes - accounted)
            {
                return lux::cxx::unexpected(failure(EWorldMaterializeError::CAPACITY, index, data));
            }
            accounted += value->accountedBytes();
            values.push_back({planned->entities()[index], std::move(*value)});
        }
    }

    // All normal rejection precedes the first Registry mutation. The caller owns
    // structural exclusion through install; callbacks must not mutate structure.
    // This is not a general transaction for arbitrary observer side effects.
    if (!ecs::validateEntityCreation(registry, *planned))
    {
        return lux::cxx::unexpected(failure(EWorldMaterializeError::STRUCTURE_CHANGED));
    }
    identities.reserve(identities.size() + input.size());
    std::vector<ecs::Entity> result(planned->entities().begin(), planned->entities().end());
    for (std::size_t index{}; index < input.size(); ++index)
    {
        if (registry.create() != result[index] || !identities.bind(input[index].id(), result[index]))
        {
            std::terminate(); // Structural exclusion/provider contract violation.
        }
    }
    for (auto &prepared : values)
    {
        std::move(prepared.value).installInto(registry, prepared.entity);
    }
    if (created)
    {
        *created = std::move(result);
    }
    if (component_bytes)
    {
        *component_bytes = accounted;
    }
    return {};
}
} // namespace lux::scene
