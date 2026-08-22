#include <lux/engine/editor/scene/WorldActorEcsAdapter.hpp>

#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/ecs/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>
#include <lux/engine/ecs/Registry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <type_traits>
#include <unordered_map>

namespace lux::editor
{
    namespace
    {
        uuids::uuid randomUuid()
        {
            std::random_device device;
            std::array<int, std::mt19937::state_size> seed{};
            std::generate(seed.begin(), seed.end(), std::ref(device));
            std::seed_seq sequence(seed.begin(), seed.end());
            std::mt19937 generator(sequence);
            return uuids::uuid_random_generator{generator}();
        }

        [[nodiscard]] lux::authoring::WorldActorId
        toAuthoringId(const lux::ecs::PersistentEntityId& id) noexcept
        {
            return lux::authoring::WorldActorId{id.value()};
        }

        [[nodiscard]] lux::ecs::PersistentEntityId
        toRuntimeId(const lux::authoring::WorldActorId& id) noexcept
        {
            return lux::ecs::PersistentEntityId{id.value()};
        }

        static_assert(!std::is_constructible_v<
            lux::authoring::WorldActorId,
            lux::ecs::PersistentEntityId>);
        static_assert(!std::is_constructible_v<
            lux::ecs::PersistentEntityId,
            lux::authoring::WorldActorId>);
        static_assert(!std::is_assignable_v<
            lux::authoring::WorldActorId&,
            lux::ecs::PersistentEntityId>);
        static_assert(!std::is_assignable_v<
            lux::ecs::PersistentEntityId&,
            lux::authoring::WorldActorId>);
    } // namespace

    lux::cxx::expected<lux::authoring::WorldActorDocument, std::string>
    WorldActorEcsAdapter::capture(
        lux::ecs::Registry& registry,
        entt::entity entity,
        lux::authoring::WorldId world,
        std::string_view origin)
    {
        if (!persistent_entities_.boundTo(registry))
        {
            return lux::cxx::unexpected(
                std::string{"PersistentEntityIndex registry mismatch in '"}
                + std::string{origin} + "'");
        }
        if (!registry.valid(entity) || world.empty())
            return lux::cxx::unexpected(
                std::string{"invalid ECS Actor or World for '"}
                + std::string{origin} + "'");
        const auto* hierarchy = registry.try_get<
            lux::ecs::ParentComponent>(entity);
        const bool has_2d = registry.any_of<
            lux::ecs::Transform2DComponent>(entity);
        const bool has_3d = registry.any_of<
            lux::ecs::Transform3DComponent>(entity);
        if (has_2d == has_3d)
        {
            return lux::cxx::unexpected(
                std::string{"Actor requires exactly one Transform in '"}
                + std::string{origin} + "'");
        }
        if (has_2d)
        {
            const auto* transform = registry.try_get<
                lux::ecs::Transform2DComponent>(entity);
            if (!transform || registry.any_of<
                    lux::ecs::Transform3DComponent>(entity))
            {
                return lux::cxx::unexpected(
                    std::string{"2D Actor requires exactly one 2D Transform in '"}
                    + std::string{origin} + "'");
            }
        }
        else
        {
            const auto* transform = registry.try_get<
                lux::ecs::Transform3DComponent>(entity);
            if (!transform || registry.any_of<
                    lux::ecs::Transform2DComponent>(entity))
            {
                return lux::cxx::unexpected(
                    std::string{"3D Actor requires exactly one 3D Transform in '"}
                    + std::string{origin} + "'");
            }
        }

        auto* stable = registry.try_get<lux::ecs::PersistentEntityIdComponent>(
            entity);
        if (!stable || stable->id().empty())
        {
            const auto assigned = lux::ecs::setPersistentEntityId(
                persistent_entities_,
                entity,
                lux::ecs::PersistentEntityId{randomUuid()});
            if (!assigned)
            {
                return lux::cxx::unexpected(
                    std::string{"cannot assign persistent Actor identity in '"}
                    + std::string{origin} + "'");
            }
            stable = registry.try_get<
                lux::ecs::PersistentEntityIdComponent>(entity);
        }

        lux::serialize::NameTable names;
        lux::authoring::WorldActorDocument document;
        document.world = world;
        document.actor = toAuthoringId(stable->id());
        if (hierarchy)
        {
            auto* parent = registry.try_get<
                lux::ecs::PersistentEntityIdComponent>(hierarchy->parent());
            if (!parent && registry.valid(hierarchy->parent()))
            {
                const auto assigned = lux::ecs::setPersistentEntityId(
                    persistent_entities_,
                    hierarchy->parent(),
                    lux::ecs::PersistentEntityId{randomUuid()});
                if (!assigned)
                {
                    return lux::cxx::unexpected(
                        std::string{"cannot assign persistent parent identity in '"}
                        + std::string{origin} + "'");
                }
                parent = registry.try_get<
                    lux::ecs::PersistentEntityIdComponent>(
                        hierarchy->parent());
            }
            if (!parent || parent->id().empty())
            {
                return lux::cxx::unexpected(
                    std::string{"persistent World child has no stable parent in '"}
                    + std::string{origin} + "'");
            }
            const auto parent_id = toAuthoringId(parent->id());
            document.transform_parent = parent_id;
            document.references.push_back({
                parent_id,
                lux::authoring::EWorldActorReferenceKind::LOCAL});
        }

        if (has_2d)
        {
            const auto* source = hierarchy
                ? static_cast<const lux::math::Position2d*>(nullptr)
                : &registry.get<lux::ecs::Transform2DComponent>(entity).position;
            if (hierarchy)
            {
                const auto* resolved = registry.try_get<
                    lux::ecs::ResolvedTransform2DComponent>(entity);
                if (!resolved)
                    return lux::cxx::unexpected(
                        std::string{"2D child has no resolved Transform in '"} +
                        std::string{origin} + "'");
                source = &resolved->position;
            }
            if (!lux::math::isFinite(*source))
                return lux::cxx::unexpected(
                    std::string{"2D Actor position is invalid in '"} +
                    std::string{origin} + "'");
            document.position = *source;
        }
        else
        {
            const auto* source = hierarchy
                ? static_cast<const lux::math::Position3d*>(nullptr)
                : &registry.get<lux::ecs::Transform3DComponent>(entity).position;
            if (hierarchy)
            {
                const auto* resolved = registry.try_get<
                    lux::ecs::ResolvedTransform3DComponent>(entity);
                if (!resolved)
                    return lux::cxx::unexpected(
                        std::string{"3D child has no resolved Transform in '"} +
                        std::string{origin} + "'");
                source = &resolved->position;
            }
            if (!lux::math::isFinite(*source))
                return lux::cxx::unexpected(
                    std::string{"3D Actor position is invalid in '"} +
                    std::string{origin} + "'");
            document.position = *source;
        }
        for (const auto& schema : components_.all())
        {
            // Runtime-derived state is deliberately discoverable through the
            // component catalogue, but it is not Authoring content. Filter it
            // before reflection/serialization validation: a transient schema
            // is allowed to have no RefClass because no LXAD writer may ever
            // inspect or encode it.
            if (schema.serialization ==
                lux::ecs::EComponentSerializationPolicy::TRANSIENT)
            {
                continue;
            }
            if (schema.cpp_type == lux::cxx::typeToken<
                    lux::ecs::PersistentEntityIdComponent>() ||
                schema.cpp_type == lux::cxx::typeToken<
                    lux::ecs::ParentComponent>())
            {
                continue;
            }
            if (!schema.operations.has || !schema.has(registry, entity))
                continue;
            if (!schema.ref_class || !schema.operations.get)
            {
                return lux::cxx::unexpected(
                    std::string{"component schema cannot be authored: '"}
                    + std::string{schema.fullName()} + "'");
            }
            lux::authoring::WorldActorComponentRecord record;
            record.schema_name = schema.fullName();
            record.schema_version = schema.schema_version;
            lux::serialize::ArchiveWriter writer{record.tagged_payload};
            void* component = schema.operations.get(registry, entity);
            if (!component)
                return lux::cxx::unexpected(
                    std::string{"component disappeared while authoring '"}
                    + std::string{origin} + "'");
            lux::ecs::serialization::TaggedPropertyWriter tagged{writer, names};
            const auto encoded =
                tagged.writeObject(*schema.ref_class, component);
            if (!encoded)
            {
                return lux::cxx::unexpected(
                    std::string{"component archive encode failed for '"}
                    + std::string{schema.fullName()} + "': "
                    + encoded.error().detail);
            }
            document.components.push_back(std::move(record));
        }
        {
            lux::serialize::ArchiveWriter writer{document.name_table};
            names.serialize(writer);
        }
        return document;
    }

    lux::cxx::expected<entt::entity, std::string>
    WorldActorEcsAdapter::materialize(
        const lux::authoring::WorldActorDocument& document,
        lux::ecs::Registry& registry,
        std::string_view origin) const
    {
        if (!persistent_entities_.boundTo(registry))
        {
            return lux::cxx::unexpected(
                std::string{"PersistentEntityIndex registry mismatch in '"}
                + std::string{origin} + "'");
        }
        std::unordered_map<
            std::string_view,
            const lux::ecs::ComponentSchemaDescriptor*> schemas;
        schemas.reserve(components_.all().size());
        for (const auto& schema : components_.all())
            schemas.emplace(schema.fullName(), &schema);

        for (const auto& record : document.components)
        {
            const auto found = schemas.find(record.schema_name);
            if (found == schemas.end() || !found->second->ref_class
                || !found->second->operations.emplace
                || found->second->cpp_type == lux::cxx::typeToken<
                    lux::ecs::PersistentEntityIdComponent>()
                || found->second->cpp_type == lux::cxx::typeToken<
                    lux::ecs::ParentComponent>()
                || found->second->schema_version != record.schema_version)
            {
                return lux::cxx::unexpected(
                    std::string{"MISSING_OR_MISMATCHED_COMPONENT_SCHEMA: '"}
                    + record.schema_name + "' in '" + std::string{origin}
                    + "'");
            }
        }

        lux::serialize::ArchiveReader name_reader{
            document.name_table.data(), document.name_table.size()};
        auto names = lux::serialize::NameTable::deserialize(name_reader);
        if (!name_reader.ok() || !name_reader.eof())
            return lux::cxx::unexpected(
                std::string{"invalid LXAD NameTable in '"}
                + std::string{origin} + "'");

        const auto entity = registry.create();
        for (const auto& record : document.components)
        {
            const auto* schema = schemas.at(record.schema_name);
            void* component = schema->operations.emplace(registry, entity);
            if (!component)
            {
                registry.destroy(entity);
                return lux::cxx::unexpected(
                    std::string{"component emplace failed for '"}
                    + record.schema_name + "'");
            }
            lux::serialize::ArchiveReader payload_reader{
                record.tagged_payload.data(), record.tagged_payload.size()};
            lux::ecs::serialization::TaggedPropertyReader tagged{payload_reader, names};
            const auto decoded =
                tagged.readObject(*schema->ref_class, component);
            if (!decoded || !payload_reader.ok() || !payload_reader.eof())
            {
                registry.destroy(entity);
                return lux::cxx::unexpected(
                    std::string{"invalid tagged component payload for '"}
                    + record.schema_name + "': "
                    + (decoded ? std::string{"trailing bytes"}
                               : decoded.error().detail));
            }
        }
        const auto assigned = lux::ecs::setPersistentEntityId(
            persistent_entities_, entity, toRuntimeId(document.actor));
        if (!assigned)
        {
            registry.destroy(entity);
            return lux::cxx::unexpected(
                std::string{"invalid or duplicate persistent Actor identity in '"}
                + std::string{origin} + "'");
        }

        if (const auto* position = std::get_if<lux::math::Position2d>(
                &document.position))
        {
            auto* transform = registry.try_get<
                lux::ecs::Transform2DComponent>(entity);
            if (!transform || registry.any_of<
                    lux::ecs::Transform3DComponent>(entity))
            {
                registry.destroy(entity);
                return lux::cxx::unexpected(
                    std::string{"2D Actor must contain exactly one 2D Transform in '"} +
                    std::string{origin} + "'");
            }
            if (!document.transform_parent)
            {
                if (!lux::math::isFinite(*position))
                {
                    registry.destroy(entity);
                    return lux::cxx::unexpected(
                        std::string{"invalid 2D Actor position in '"} +
                        std::string{origin} + "'");
                }
                transform->position = *position;
                registry.patch<lux::ecs::Transform2DComponent>(entity);
            }
        }
        else
        {
            auto* transform = registry.try_get<
                lux::ecs::Transform3DComponent>(entity);
            if (!transform || registry.any_of<
                    lux::ecs::Transform2DComponent>(entity))
            {
                registry.destroy(entity);
                return lux::cxx::unexpected(
                    std::string{"3D Actor must contain exactly one 3D Transform in '"} +
                    std::string{origin} + "'");
            }
            if (!document.transform_parent)
            {
                const auto* position = std::get_if<
                    lux::math::Position3d>(&document.position);
                if (!position || !lux::math::isFinite(*position))
                {
                    registry.destroy(entity);
                    return lux::cxx::unexpected(
                        std::string{"invalid 3D Actor position in '"} +
                        std::string{origin} + "'");
                }
                transform->position = *position;
                registry.patch<lux::ecs::Transform3DComponent>(entity);
            }
        }

        if (document.transform_parent)
        {
            const auto parent = persistent_entities_.find(
                toRuntimeId(*document.transform_parent));
            if (parent != entt::null)
            {
                if (parent == entity ||
                    !lux::ecs::setParent(registry, entity, parent))
                {
                    registry.destroy(entity);
                    return lux::cxx::unexpected(
                        std::string{"World Actor Transform hierarchy is cyclic in '"}
                        + std::string{origin} + "'");
                }
            }
            else
            {
                registry.destroy(entity);
                return lux::cxx::unexpected(
                    std::string{"World Actor parent must be materialized first in '"}
                    + std::string{origin} + "'");
            }
        }
        return entity;
    }
} // namespace lux::editor
