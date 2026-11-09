#include <lux/engine/scene/SceneContent.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/world/World.hpp>
#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

namespace lux::scene
{
    namespace
    {
        constexpr auto kInvalid = error::errorId("lux.scene.content.invalid");
        constexpr auto kCapacity = error::errorId("lux.scene.content.capacity");
        constexpr auto kSchema = error::errorId("lux.scene.content.schema");
        constexpr auto kRestore = error::errorId("lux.scene.content.restore");

        bool validateGeometryBounds(const spatial::Spatial3DQueryBounds& bounds) noexcept
        {
            return math::isFinite(bounds.minimum) && math::isFinite(bounds.maximum) &&
                bounds.minimum.x <= bounds.maximum.x && bounds.minimum.y <= bounds.maximum.y &&
                bounds.minimum.z <= bounds.maximum.z;
        }

        bool validateSummary(std::span<const SceneObjectSummary> summaries) noexcept
        {
            std::unordered_set<world::WorldObjectId, world::WorldObjectIdHash> ids;
            ids.reserve(summaries.size());
            for (const auto& summary : summaries)
            {
                const bool invalid_identity = !world::isValid(summary.id) || !ids.insert(summary.id).second;
                if (invalid_identity)
                {
                    return false;
                }
                for (const auto& geometry : summary.geometry)
                {
                    if (geometry.geometry == UINT32_MAX || !validateGeometryBounds(geometry.bounds)) return false;
                }
            }
            return true;
        }
    }

    cxx::expected<void, error::Error>
    encodeSceneSummary(
        std::span<const SceneObjectSummary> summaries, std::vector<std::byte>& output, std::size_t limit
    ) noexcept
    {
        if (!validateSummary(summaries)) return cxx::unexpected(error::Error{kInvalid});
        // Charge the exact wire size before allocating the candidate.
        std::size_t bytes = sizeof(std::uint64_t);
        const auto charge = [&](std::size_t count, std::size_t stride) noexcept
        {
            if (bytes > limit || (count && stride > (limit - bytes) / count)) return false;
            bytes += count * stride;
            return true;
        };
        for (const auto& summary : summaries)
        {
            if (!charge(1, 24) || !charge(summary.geometry.size(), 52))
                return cxx::unexpected(error::Error{kCapacity});
        }
        if (bytes > limit) return cxx::unexpected(error::Error{kCapacity});
        std::vector<std::byte> candidate;
        candidate.reserve(bytes);
        serialization::BinaryWriter writer(candidate);
        const serialization::SerializationBudget budget{limit, limit, 32};
        if (!writer.writeUnsigned(static_cast<std::uint64_t>(summaries.size())))
            return cxx::unexpected(error::Error{kCapacity});
        for (const auto& summary : summaries)
        {
            if (!writer.writeBytes(summary.id.value.as_bytes()))
                return cxx::unexpected(error::Error{kCapacity});
            if (!writer.writeUnsigned(static_cast<std::uint64_t>(summary.geometry.size())))
                return cxx::unexpected(error::Error{kCapacity});
            for (const auto& geometry : summary.geometry)
            {
                const auto& low = geometry.bounds.minimum;
                const auto& high = geometry.bounds.maximum;
                const auto fields = std::tie(geometry.geometry, low.x, low.y, low.z, high.x, high.y, high.z);
                if (!serialization::write(writer, fields, budget)) return cxx::unexpected(error::Error{kCapacity});
            }
        }
        output = std::move(candidate);
        return {};
    }

    cxx::expected<std::vector<SceneObjectSummary>, error::Error>
    decodeSceneSummary(std::span<const std::byte> bytes, std::size_t limit) noexcept
    {
        if (bytes.size() > limit) return cxx::unexpected(error::Error{kCapacity});
        serialization::BinaryReader reader(bytes);
        auto count = reader.readUnsigned<std::uint64_t>();
        if (!count || *count > reader.remaining() / 24) return cxx::unexpected(error::Error{kInvalid});
        std::size_t charged = bytes.size();
        const auto charge = [&](std::uint64_t count, std::size_t stride) noexcept
        {
            if (count > SIZE_MAX || (count && stride > (limit - charged) / count)) return false;
            charged += static_cast<std::size_t>(count) * stride;
            return true;
        };
        if (!charge(*count, sizeof(SceneObjectSummary))) return cxx::unexpected(error::Error{kCapacity});
        std::vector<SceneObjectSummary> summaries(static_cast<std::size_t>(*count));
        const serialization::SerializationBudget budget{limit, limit, 32};
        for (auto& summary : summaries)
        {
            std::array<std::uint8_t, 16> identity{};
            if (!reader.readBytes(std::as_writable_bytes(std::span(identity))))
                return cxx::unexpected(error::Error{kInvalid});
            summary.id = world::WorldObjectId{uuids::uuid(identity)};
            auto geometry = reader.readUnsigned<std::uint64_t>();
            if (!geometry || *geometry > reader.remaining() / 52) return cxx::unexpected(error::Error{kInvalid});
            if (!charge(*geometry, sizeof(SceneGeometryBounds))) return cxx::unexpected(error::Error{kCapacity});
            summary.geometry.resize(static_cast<std::size_t>(*geometry));
            for (auto& range : summary.geometry)
            {
                auto& low = range.bounds.minimum;
                auto& high = range.bounds.maximum;
                auto fields = std::tie(range.geometry, low.x, low.y, low.z, high.x, high.y, high.z);
                if (!serialization::read(reader, fields, budget)) return cxx::unexpected(error::Error{kInvalid});
            }
        }
        if (reader.remaining() || !validateSummary(summaries)) return cxx::unexpected(error::Error{kInvalid});
        return summaries;
    }

    cxx::expected<void, error::Error>
    encodeSceneBlock(
        std::span<const SceneObjectData> objects, std::span<const SceneObjectSummary> summaries,
        std::vector<std::byte>& output, SceneBlockDescription& description, std::size_t limit
    ) noexcept
    {
        if (objects.size() != summaries.size()) return cxx::unexpected(error::Error{kInvalid});
        std::vector<std::byte> candidate;
        auto summary = encodeSceneSummary(summaries, candidate, limit);
        if (!summary) return summary;
        const auto summary_size = candidate.size();
        const auto summary_digest = cxx::algorithm::Sha256::hash(std::span(candidate));
        std::size_t bytes = candidate.size();
        const auto charge = [&](std::size_t amount) noexcept
        {
            if (amount > limit - bytes) return false;
            bytes += amount;
            return true;
        };
        for (std::size_t i = 0; i < objects.size(); ++i)
        {
            const auto& object = objects[i];
            if (object.id != summaries[i].id) return cxx::unexpected(error::Error{kInvalid, {i}});
            if (!charge(8)) return cxx::unexpected(error::Error{kCapacity});
            std::unordered_set<std::string_view> names;
            for (const auto& component : object.components)
            {
                const bool invalid = !component.schema.valid() || !component.version ||
                    !names.insert(component.schema.name).second;
                if (invalid) return cxx::unexpected(error::Error{kSchema, {i}});
                if (!charge(20) || !charge(component.schema.name.size()) || !charge(component.payload.size()))
                    return cxx::unexpected(error::Error{kCapacity});
            }
        }
        candidate.reserve(bytes);
        serialization::BinaryWriter writer(candidate);
        const serialization::SerializationBudget budget{limit, limit, 32};
        for (const auto& object : objects)
        {
            if (!writer.writeUnsigned(static_cast<std::uint64_t>(object.components.size())))
                return cxx::unexpected(error::Error{kCapacity});
            for (const auto& component : object.components)
            {
                const auto fields = std::tie(component.schema.name, component.version, component.payload);
                if (!serialization::write(writer, fields, budget)) return cxx::unexpected(error::Error{kCapacity});
            }
        }
        auto recorded = description;
        recorded.summary_size = summary_size;
        recorded.size = candidate.size();
        recorded.summary_digest = summary_digest;
        recorded.digest = cxx::algorithm::Sha256::hash(std::span(candidate));
        output = std::move(candidate);
        description = recorded;
        return {};
    }

    cxx::expected<std::vector<SceneObjectData>, error::Error>
    decodeSceneBlock(
        std::span<const std::byte> bytes, const SceneBlockDescription& description, std::size_t limit
    ) noexcept
    {
        const bool invalid_size = description.size != bytes.size() || !description.summary_size ||
            description.summary_size > bytes.size();
        if (invalid_size) return cxx::unexpected(error::Error{kInvalid});
        if (bytes.size() > limit) return cxx::unexpected(error::Error{kCapacity});
        const auto summary_bytes = bytes.first(static_cast<std::size_t>(description.summary_size));
        const bool mismatch = cxx::algorithm::Sha256::hash(bytes) != description.digest ||
            cxx::algorithm::Sha256::hash(summary_bytes) != description.summary_digest;
        if (mismatch) return cxx::unexpected(error::Error{kInvalid});
        auto summaries = decodeSceneSummary(summary_bytes, limit);
        if (!summaries) return cxx::unexpected(summaries.error());
        // Body values are associated by summary order: UUIDs are not stored twice.
        serialization::BinaryReader reader(bytes.subspan(summary_bytes.size()));
        std::size_t charged = bytes.size();
        const auto charge = [&](std::size_t count, std::size_t size) noexcept
        {
            if (count && size > (limit - charged) / count) return false;
            charged += count * size;
            return true;
        };
        if (!charge(summaries->size(), sizeof(SceneObjectData))) return cxx::unexpected(error::Error{kCapacity});
        for (const auto& summary : *summaries)
        {
            if (!charge(1, sizeof(SceneObjectSummary)) ||
                !charge(summary.geometry.size(), sizeof(SceneGeometryBounds)))
                return cxx::unexpected(error::Error{kCapacity});
        }
        std::vector<SceneObjectData> objects(summaries->size());
        const serialization::SerializationBudget budget{bytes.size(), bytes.size(), 32};
        for (std::size_t i = 0; i < objects.size(); ++i)
        {
            auto& object = objects[i];
            object.id = (*summaries)[i].id;
            auto count = reader.readUnsigned<std::uint64_t>();
            if (!count || *count > reader.remaining() / 20) return cxx::unexpected(error::Error{kInvalid, {i}});
            if (*count > SIZE_MAX || !charge(static_cast<std::size_t>(*count), sizeof(SceneComponentData)))
                return cxx::unexpected(error::Error{kCapacity});
            object.components.resize(static_cast<std::size_t>(*count));
            std::unordered_set<std::string_view> names;
            for (auto& component : object.components)
            {
                auto fields = std::tie(component.schema.name, component.version, component.payload);
                if (!serialization::read(reader, fields, budget)) return cxx::unexpected(error::Error{kInvalid, {i}});
                component.schema.hash = cxx::Fnv1a64::hash(component.schema.name);
                const bool invalid = !component.schema.valid() || !component.version ||
                    !names.insert(component.schema.name).second;
                if (invalid) return cxx::unexpected(error::Error{kSchema, {i}});
            }
        }
        if (reader.remaining()) return cxx::unexpected(error::Error{kInvalid});
        return objects;
    }

    cxx::expected<void, error::Error>
    captureSceneBlock(
        world::World& world, std::span<const ecs::Entity> entities, const ecs::ComponentSchemaSet& schemas,
        std::vector<SceneObjectData>& output
    ) noexcept
    {
        const auto& registry = world.registry();
        std::vector<SceneObjectData> candidate;
        candidate.reserve(entities.size());
        std::unordered_set<ecs::Entity> unique;
        // Only the caller's changed block is examined, never all resident entities for each schema.
        // O(changed objects * registered persistent schemas + encoded bytes), on the cold save path.
        for (const auto entity : entities)
        {
            const auto* id = registry.valid(entity) ? registry.try_get<world::WorldObjectId>(entity) : nullptr;
            const bool is_invalid_object = !id || !world::isValid(*id) || !unique.insert(entity).second;
            if (is_invalid_object)
                return cxx::unexpected(error::Error{kInvalid});
            SceneObjectData object{*id, {}};
            for (const auto& schema : schemas.all())
            {
                if (!schema.persistent || !schema.operations.has(registry, entity)) continue;
                if (!schema.encode || !schema.visit_references) return cxx::unexpected(error::Error{kSchema});
                bool has_entity_reference = false;
                const ecs::ComponentEntityVisitor visitor{
                    &has_entity_reference,
                    +[](void* state, ecs::Entity target) noexcept
                    {
                        if (target != entt::null) *static_cast<bool*>(state) = true;
                    }
                };
                const auto visited = schema.visit_references(registry, entity, visitor);
                if (!visited || has_entity_reference) return cxx::unexpected(error::Error{kSchema});
                SceneComponentData component{schema.id, schema.version, {}};
                auto encoded = schema.encode(
                    registry, entity, component.payload,
                    serialization::SerializationBudget{SIZE_MAX, SIZE_MAX, 64}
                );
                if (!encoded)
                {
                    return cxx::unexpected(error::Error{kSchema,
                        {static_cast<std::uint64_t>(encoded.error().code), encoded.error().offset}});
                }
                object.components.push_back(std::move(component));
            }
            candidate.push_back(std::move(object));
        }
        output = std::move(candidate);
        return {};
    }

    cxx::expected<std::vector<ecs::Entity>, error::Error>
    restoreSceneGroup(
        world::World& world, std::span<const SceneObjectData> objects, const ecs::ComponentSchemaSet& schemas
    ) noexcept
    {
        std::size_t count{};
        for (const auto& object : objects)
        {
            if (object.components.size() > SIZE_MAX - count) return cxx::unexpected(error::Error{kCapacity});
            count += object.components.size();
        }
        std::vector<WorldComponentInput> fields;
        fields.reserve(count);
        std::vector<WorldObjectInput> input;
        input.reserve(objects.size());
        for (const auto& object : objects)
        {
            const auto first = fields.size();
            for (const auto& component : object.components)
            {
                const auto* schema = schemas.find(component.schema);
                const bool unavailable = !schema || !schema->persistent || !schema->decode_value;
                if (unavailable) return cxx::unexpected(error::Error{kSchema});
                fields.push_back({schema, component.version, component.payload});
            }
            input.push_back({object.id, std::span(fields).subspan(first)});
        }
        auto prepared = WorldMaterializer::prepareObjects(world.registry(), world.identities(), input);
        if (!prepared)
        {
            const auto& failure = prepared.error();
            return cxx::unexpected(error::Error{kRestore,
                {static_cast<std::uint64_t>(failure.code), failure.object, failure.data}});
        }
        if (!prepared->references().empty()) return cxx::unexpected(error::Error{kRestore});
        std::vector<ecs::Entity> created(prepared->entities().begin(), prepared->entities().end());
        prepared->commit(world.registry(), world.identities());
        return created;
    }
} // namespace lux::scene
