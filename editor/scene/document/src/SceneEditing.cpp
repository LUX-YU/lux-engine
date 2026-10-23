#include <lux/engine/editor/SceneEditing.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace lux::editor::detail
{
    namespace ecs = simulation::ecs;

    namespace
    {
        constexpr std::size_t payload_limit = 64U << 20U;

        world::WorldPartitionObjectView sourceObject(
            const scene::WorldResidency& residency,
            partition::PartitionOrdinal partition,
            world::WorldObjectId id
        ) noexcept
        {
            const auto* source = residency.source(partition);
            if (!source)
            {
                return {};
            }
            const world::WorldObjectIdLess less;
            std::size_t first = 0, last = source->objectCount();
            while (first < last)
            {
                const auto middle = first + (last - first) / 2;
                if (less(source->objectAt(middle).id(), id))
                {
                    first = middle + 1;
                }
                else
                {
                    last = middle;
                }
            }
            if (first < source->objectCount() && source->objectAt(first).id() == id)
            {
                return source->objectAt(first);
            }
            return {};
        }

        SceneEditResult<SceneComponentData> captureComponent(
            const ecs::Registry& registry,
            const ecs::WorldEntityMap& identities,
            const ecs::ComponentSchema& schema,
            ecs::Entity entity
        ) noexcept
        {
            if (!schema.capture || schema.snapshot != ecs::EComponentSnapshotPolicy::COPY)
            {
                return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
            }
            auto value = schema.capture(registry, entity, schema.code_lifetime);
            if (!value)
            {
                return cxx::unexpected(VSceneEditFailure{value.error()});
            }
            auto bytes = value->encode(identities, payload_limit);
            if (!bytes)
            {
                return cxx::unexpected(VSceneEditFailure{bytes.error()});
            }
            return SceneComponentData{schema.id, schema.version, std::move(*bytes)};
        }

        bool permitsAuthorValue(const scene::WorldResidency& residency, const ecs::ComponentSchema& schema) noexcept
        {
            const bool authored = schema.snapshot == ecs::EComponentSnapshotPolicy::COPY &&
                                  schema.semantic_kind != ecs::EComponentSemanticKind::RUNTIME_DERIVED;
            return authored && std::ranges::any_of(
                                   residency.description().schemas(),
                                   [&](const auto& item) noexcept { return item.name == schema.id.name; }
                               );
        }

        // Prepared parent edges replace existing edges only for this cold edit; no persistent hierarchy copy.
        bool acyclicParents(
            const ecs::Registry& registry,
            const std::unordered_map<ecs::Entity, ecs::Entity>& changed
        ) noexcept
        {
            std::unordered_set<ecs::Entity> complete;
            std::unordered_set<ecs::Entity> path;
            for (const auto& [entity, parent] : changed)
            {
                auto current = entity;
                path.clear();
                while (current != ecs::NullEntity && !complete.contains(current))
                {
                    if (!path.insert(current).second)
                    {
                        return false;
                    }
                    if (const auto edge = changed.find(current); edge != changed.end())
                    {
                        current = edge->second;
                    }
                    else
                    {
                        const auto* value = registry.try_get<const ecs::Parent>(current);
                        current = value ? value->entity : ecs::NullEntity;
                    }
                }
                complete.insert(path.begin(), path.end());
            }
            return true;
        }

        struct ResolvedValue final
        {
            ecs::Entity entity;
            const ecs::ComponentSchema* schema;
            std::optional<ecs::DecodedComponent> value;
            bool existed{};
        };

        SceneEditResult<VSceneEdit> writeComponents(
            ecs::Registry& registry,
            scene::WorldResidency& residency,
            const ecs::ComponentSchemaSet& schemas,
            const SceneWriteComponents& write
        ) noexcept
        {
            std::vector<ResolvedValue> prepared;
            SceneWriteComponents inverse;
            std::set<std::pair<std::uint64_t, std::string>> writes;
            std::unordered_map<ecs::Entity, ecs::Entity> parents;
            std::set<std::uint32_t> dirty;
            std::size_t additional_bytes = 0;
            const ecs::ComponentEntityResolver resolver{
                &residency.identities(),
                +[](const void* state,
                    world::WorldObjectId id) noexcept -> cxx::expected<ecs::Entity, ecs::ComponentDecodeFailure>
                {
                    if (!id.valid())
                    {
                        return ecs::NullEntity;
                    }
                    const auto entity = static_cast<const ecs::WorldEntityMap*>(state)->entity(id);
                    if (entity == ecs::NullEntity)
                    {
                        return cxx::unexpected(
                            ecs::ComponentDecodeFailure{ecs::EComponentDecodeError::UNRESOLVED_REFERENCE, 0, id}
                        );
                    }
                    return entity;
                }
            };
            for (const auto& edit : write.components)
            {
                const auto entity = residency.identities().entity(edit.object);
                partition::PartitionOrdinal partition;
                if (!registry.valid(entity) || !residency.partitionOf(entity, partition))
                {
                    return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_OBJECT});
                }
                const auto* schema = schemas.find(edit.schema);
                const bool valid_schema = schema && permitsAuthorValue(residency, *schema) && schema->decode_value;
                const bool matching_value = !edit.value || edit.value->schema == edit.schema;
                if (!valid_schema || !matching_value)
                {
                    return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
                }
                if (!writes.emplace(ecs::entityBits(entity), schema->id.name).second)
                {
                    return cxx::unexpected(VSceneEditFailure{ESceneEditError::DUPLICATE_WRITE});
                }
                const bool existed = schema->operations.has(registry, entity);
                SceneComponentEdit restore{edit.object, edit.schema};
                if (existed)
                {
                    auto previous = captureComponent(registry, residency.identities(), *schema, entity);
                    if (!previous)
                    {
                        return cxx::unexpected(previous.error());
                    }
                    restore.value = std::move(*previous);
                }
                inverse.components.push_back(std::move(restore));
                ResolvedValue operation{entity, schema, {}, existed};
                if (edit.value)
                {
                    auto value =
                        schema->decode_value(edit.value->version, edit.value->payload, resolver, schema->code_lifetime);
                    if (!value)
                    {
                        return cxx::unexpected(VSceneEditFailure{value.error()});
                    }
                    if (existed && !value->canReplace())
                    {
                        return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
                    }
                    if (!existed)
                    {
                        additional_bytes += value->accountedBytes();
                    }
                    operation.value.emplace(std::move(*value));
                }
                if (schema->cpp_type == cxx::typeToken<ecs::Parent>())
                {
                    parents[entity] = operation.value ? static_cast<const ecs::Parent*>(operation.value->data())->entity
                                                      : ecs::NullEntity;
                }
                prepared.push_back(std::move(operation));
                dirty.insert(partition.value);
            }
            if (!acyclicParents(registry, parents))
            {
                return cxx::unexpected(VSceneEditFailure{ESceneEditError::HIERARCHY_CYCLE});
            }
            if (auto capacity = residency.validateAdditional(0, additional_bytes); !capacity)
            {
                return cxx::unexpected(VSceneEditFailure{capacity.error()});
            }
            // No recoverable validation follows this point. Typed replace publishes on_update.
            for (auto& edit : prepared)
            {
                if (!edit.value)
                {
                    edit.schema->operations.erase(registry, edit.entity);
                }
                else if (edit.existed)
                {
                    std::move(*edit.value).replaceInto(registry, edit.entity);
                }
                else
                {
                    std::move(*edit.value).installInto(registry, edit.entity);
                }
            }
            for (auto partition : dirty)
            {
                static_cast<void>(residency.setDirty({partition}, true));
            }
            std::vector<ecs::Entity> changed;
            changed.reserve(prepared.size());
            for (const auto& edit : prepared)
            {
                changed.push_back(edit.entity);
            }
            std::ranges::sort(changed);
            changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
            residency.synchronizeComponents(changed);
            return VSceneEdit{std::move(inverse)};
        }
    } // namespace

    SceneEditResult<SceneObjectData> captureSceneObject(
        const ecs::Registry& registry,
        const scene::WorldResidency& residency,
        const ecs::ComponentSchemaSet& schemas,
        world::WorldObjectId id
    ) noexcept
    {
        SceneObjectData result{id};
        const auto entity = residency.identities().entity(id);
        if (!registry.valid(entity) || !residency.partitionOf(entity, result.partition))
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_OBJECT});
        }
        for (const auto& schema : schemas.all())
        {
            if (!permitsAuthorValue(residency, schema) || !schema.operations.has(registry, entity))
            {
                continue;
            }
            auto captured = captureComponent(registry, residency.identities(), schema, entity);
            if (!captured)
            {
                return cxx::unexpected(captured.error());
            }
            result.components.push_back(std::move(*captured));
        }
        const auto source = sourceObject(residency, result.partition, id);
        for (std::size_t i = 0; source && i < source.dataCount(); ++i)
        {
            const auto& identity = residency.description().schemas()[source.schemaOrdinalAt(i)];
            const auto schema = ecs::componentSchemaId(identity.name);
            if (!schemas.find(schema))
            {
                const auto payload = source.payloadAt(i);
                result.components.push_back({schema, source.schemaVersionAt(i), {payload.begin(), payload.end()}});
            }
        }
        return result;
    }

    SceneEditResult<VSceneEdit> applySceneEdit(
        ecs::Registry& registry,
        scene::WorldResidency& residency,
        const ecs::ComponentSchemaSet& schemas,
        const VSceneEdit& edit
    ) noexcept
    {
        if (const auto* write = std::get_if<SceneWriteComponents>(&edit))
        {
            return writeComponents(registry, residency, schemas, *write);
        }
        if (const auto* erase = std::get_if<SceneEraseObjects>(&edit))
        {
            SceneCreateObjects inverse;
            for (const auto id : erase->objects)
            {
                auto object = captureSceneObject(registry, residency, schemas, id);
                if (!object)
                {
                    return cxx::unexpected(object.error());
                }
                for (const auto& component : object->components)
                {
                    if (!schemas.find(component.schema))
                    {
                        return cxx::unexpected(VSceneEditFailure{ESceneEditError::UNKNOWN_PAYLOAD});
                    }
                }
                inverse.objects.push_back(std::move(*object));
            }
            auto prepared = residency.prepareErase(registry, erase->objects);
            if (!prepared)
            {
                return cxx::unexpected(VSceneEditFailure{prepared.error()});
            }
            prepared->commit();
            return VSceneEdit{std::move(inverse)};
        }
        const auto& create = std::get<SceneCreateObjects>(edit);
        std::vector<std::vector<scene::WorldComponentInput>> components(create.objects.size());
        std::vector<scene::WorldResidency::ObjectInput> objects;
        SceneEraseObjects inverse;
        for (std::size_t i = 0; i < create.objects.size(); ++i)
        {
            const auto& object = create.objects[i];
            for (const auto& value : object.components)
            {
                const auto* schema = schemas.find(value.schema);
                if (!schema || !permitsAuthorValue(residency, *schema))
                {
                    return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
                }
                components[i].push_back({schema, value.version, value.payload});
            }
            objects.push_back({object.partition, {object.id, components[i]}});
            inverse.objects.push_back(object.id);
        }
        auto prepared = residency.prepareCreate(registry, objects);
        if (!prepared)
        {
            return cxx::unexpected(VSceneEditFailure{prepared.error()});
        }
        // Parent invariants are an authoring rule, evaluated with the prepared identities before installation.
        const ecs::ComponentEntityResolver resolver{
            &*prepared,
            +[](const void* state,
                world::WorldObjectId id) noexcept -> cxx::expected<ecs::Entity, ecs::ComponentDecodeFailure>
            {
                if (!id.valid())
                {
                    return ecs::NullEntity;
                }
                const auto entity = static_cast<const scene::WorldResidency::PreparedChange*>(state)->entity(id);
                if (entity == ecs::NullEntity)
                {
                    return cxx::unexpected(
                        ecs::ComponentDecodeFailure{ecs::EComponentDecodeError::UNRESOLVED_REFERENCE, 0, id}
                    );
                }
                return entity;
            }
        };
        std::unordered_map<ecs::Entity, ecs::Entity> parents;
        for (const auto& object : create.objects)
        {
            for (const auto& component : object.components)
            {
                const auto* schema = schemas.find(component.schema);
                if (schema->cpp_type != cxx::typeToken<ecs::Parent>())
                {
                    continue;
                }
                auto value =
                    schema->decode_value(component.version, component.payload, resolver, schema->code_lifetime);
                if (!value)
                {
                    return cxx::unexpected(VSceneEditFailure{value.error()});
                }
                parents[prepared->entity(object.id)] = static_cast<const ecs::Parent*>(value->data())->entity;
            }
        }
        if (!acyclicParents(registry, parents))
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::HIERARCHY_CYCLE});
        }
        prepared->commit();
        return VSceneEdit{std::move(inverse)};
    }

    SceneEditResult<std::shared_ptr<const world::WorldPartitionData>> captureScenePartition(
        const ecs::Registry& registry,
        const scene::WorldResidency& residency,
        const ecs::ComponentSchemaSet& schemas,
        partition::PartitionOrdinal partition
    ) noexcept
    {
        const auto* source = residency.source(partition);
        if (!source)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::NOT_RESIDENT});
        }
        std::vector<SceneObjectData> captured;
        for (const auto& [id, entity] : residency.identities().entries())
        {
            partition::PartitionOrdinal location;
            if (residency.partitionOf(entity, location) && location == partition)
            {
                auto object = captureSceneObject(registry, residency, schemas, id);
                if (!object)
                {
                    return cxx::unexpected(object.error());
                }
                captured.push_back(std::move(*object));
            }
        }
        std::ranges::sort(captured, world::WorldObjectIdLess{}, &SceneObjectData::id);
        std::map<std::string_view, std::uint32_t> ordinals;
        const auto names = residency.description().schemas();
        for (std::uint32_t i = 0; i < names.size(); ++i)
        {
            ordinals.emplace(names[i].name, i);
        }
        std::vector<std::vector<world::WorldEncodedDataRecord>> values(captured.size());
        std::vector<world::WorldEncodedObjectRecord> objects;
        for (std::size_t i = 0; i < captured.size(); ++i)
        {
            for (const auto& component : captured[i].components)
            {
                values[i].push_back({ordinals.find(component.schema.name)->second, component.version, component.payload}
                );
            }
            std::ranges::sort(values[i], {}, &world::WorldEncodedDataRecord::schema_ordinal);
            objects.push_back({captured[i].id, values[i]});
        }
        auto wire = world::encodeWorldPartitionData(partition, objects);
        if (!wire)
        {
            return cxx::unexpected(VSceneEditFailure{wire.error()});
        }
        auto decoded = world::decodeWorldPartitionData(
            *wire,
            source->bundle(),
            source->generation(),
            partition,
            source->id(),
            static_cast<std::uint32_t>(names.size()),
            payload_limit
        );
        if (!decoded)
        {
            return cxx::unexpected(VSceneEditFailure{decoded.error()});
        }
        return std::make_shared<const world::WorldPartitionData>(std::move(*decoded));
    }
} // namespace lux::editor::detail
