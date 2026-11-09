#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/ecs/EntityCreationPlan.hpp>

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace lux::scene
{
    namespace
    {
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
            const ecs::Registry& registry;
            const ecs::WorldEntityMap& resident;
            std::unordered_map<world::WorldObjectId, ecs::Entity, world::WorldObjectIdHash> incoming;
            mutable std::vector<ecs::Entity> references;

            static lux::cxx::expected<ecs::Entity, ecs::ComponentDecodeFailure> resolve(
                const void* state,
                world::WorldObjectId id
            ) noexcept
            {
                const auto& self = *static_cast<const BatchIdentities*>(state);
                if (!world::isValid(id))
                {
                    return ecs::NullEntity;
                }
                if (const auto found = self.incoming.find(id); found != self.incoming.end())
                {
                    self.references.push_back(found->second);
                    return found->second;
                }
                const auto entity = self.resident.entity(id);
                if (entity != ecs::NullEntity && self.registry.valid(entity))
                {
                    self.references.push_back(entity);
                    return entity;
                }
                return lux::cxx::unexpected(
                    ecs::ComponentDecodeFailure{ecs::EComponentDecodeError::UNRESOLVED_REFERENCE, 0, id}
                );
            }
        };
    } // namespace

    struct WorldMaterializer::PreparedObjects::Impl final
    {
        ecs::EntityCreationPlan creation;
        std::unordered_map<world::WorldObjectId, ecs::Entity, world::WorldObjectIdHash> incoming;
        std::vector<ecs::Entity> references;
        std::vector<PreparedValue> values;
        std::size_t bytes{};
        const ecs::Registry* owner{};
        bool committed{};

        Impl(ecs::EntityCreationPlan plan) : creation(std::move(plan)) {}
        void commit(ecs::Registry& registry, ecs::WorldEntityMap& identities) noexcept
        {
            if (committed || owner != &registry || !ecs::validateEntityCreation(registry, creation))
                std::terminate();
            committed = true;
            identities.reserve(identities.size() + incoming.size());
            for (const auto entity : creation.entities())
                if (registry.create() != entity)
                    std::terminate();
            for (const auto& [id, entity] : incoming)
            {
                if (!identities.bind(id, entity))
                {
                    std::terminate();
                }
                registry.emplace<world::WorldObjectId>(entity, id);
            }
            for (auto& prepared : values)
                std::move(prepared.value).installInto(registry, prepared.entity);
        }
    };

    WorldMaterializer::PreparedObjects::PreparedObjects(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    WorldMaterializer::PreparedObjects::~PreparedObjects() noexcept = default;

    WorldMaterializer::PreparedObjects::PreparedObjects(PreparedObjects&&) noexcept = default;

    WorldMaterializer::PreparedObjects& WorldMaterializer::PreparedObjects::operator=(PreparedObjects&&) noexcept =
        default;
    std::span<const ecs::Entity> WorldMaterializer::PreparedObjects::entities() const noexcept
    {
        return impl_->creation.entities();
    }

    std::span<const ecs::Entity> WorldMaterializer::PreparedObjects::references() const noexcept
    {
        return impl_->references;
    }

    ecs::Entity WorldMaterializer::PreparedObjects::entity(world::WorldObjectId id) const noexcept
    {
        const auto found = impl_->incoming.find(id);
        return found == impl_->incoming.end() ? ecs::NullEntity : found->second;
    }

    std::size_t WorldMaterializer::PreparedObjects::componentBytes() const noexcept
    {
        return impl_->bytes;
    }

    void WorldMaterializer::PreparedObjects::commit(ecs::Registry& registry, ecs::WorldEntityMap& identities) noexcept
    {
        impl_->commit(registry, identities);
    }

    lux::cxx::expected<WorldMaterializer::PreparedObjects, WorldMaterializeFailure>
    WorldMaterializer::prepareObjects(
        const ecs::Registry& registry,
        const ecs::WorldEntityMap& identities,
        std::span<const WorldObjectInput> input,
        std::size_t maximum_component_bytes
    ) noexcept
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
            const auto& object = input[index];
            if (!world::isValid(object.id))
            {
                return lux::cxx::unexpected(failure(EWorldMaterializeError::INVALID_OBJECT, index));
            }
            if (identities.entity(object.id) != ecs::NullEntity ||
                !batch.incoming.emplace(object.id, planned->entities()[index]).second)
            {
                return lux::cxx::unexpected(failure(EWorldMaterializeError::DUPLICATE_OBJECT, index));
            }
        }

        std::vector<PreparedValue> values;
        std::size_t accounted{};
        const ecs::ComponentEntityResolver resolver{&batch, &BatchIdentities::resolve};
        for (std::size_t index{}; index < input.size(); ++index)
        {
            const auto& object = input[index];
            std::vector<lux::cxx::type_info> types;
            types.reserve(object.components.size());
            for (std::size_t data{}; data < object.components.size(); ++data)
            {
                const auto& component = object.components[data];
                const auto* schema = component.schema;
                if (!schema)
                {
                    continue; // The caller explicitly keeps this opaque value in its source owner.
                }
                const bool unsupported = !schema->decode_value;
                const bool duplicate_type = std::ranges::find(types, schema->cpp_type) != types.end();
                if (unsupported || duplicate_type)
                {
                    auto error = failure(EWorldMaterializeError::COMPONENT_DECODE_FAILURE, index, data);
                    error.component.code = ecs::EComponentDecodeError::UNSUPPORTED_TYPE;
                    return lux::cxx::unexpected(std::move(error));
                }
                types.push_back(schema->cpp_type);
                auto value =
                    schema->decode_value(component.version, component.payload, resolver, schema->code_lifetime);
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

        if (!ecs::validateEntityCreation(registry, *planned))
            return lux::cxx::unexpected(failure(EWorldMaterializeError::STRUCTURE_CHANGED));
        auto prepared = std::make_unique<PreparedObjects::Impl>(std::move(*planned));
        prepared->incoming = std::move(batch.incoming);
        prepared->references = std::move(batch.references);
        prepared->values = std::move(values);
        prepared->bytes = accounted;
        prepared->owner = &registry;
        return PreparedObjects{std::move(prepared)};
    }

} // namespace lux::scene
