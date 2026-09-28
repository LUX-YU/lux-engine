#include "SceneEditPreparation.hpp"
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <algorithm>
#include <unordered_set>

namespace lux::editor::scene::detail
{
    namespace ecs = simulation::ecs;

    SceneEditResult<void> decodeInto(
        ecs::Registry& destination,
        ecs::Entity entity,
        const SceneComponentData& value,
        const ecs::ComponentSchema& schema,
        const ecs::WorldEntityMap& identities
    )
    {
        const ecs::ComponentEntityResolver resolver{
            &identities,
            [](const void* owner,
               world::WorldObjectId id) noexcept -> lux::cxx::expected<ecs::Entity, ecs::ComponentDecodeFailure> {
                if (!id.valid())
                    return ecs::NullEntity;
                const auto entity = static_cast<const ecs::WorldEntityMap*>(owner)->entity(id);
                if (entity == ecs::NullEntity)
                    return lux::cxx::unexpected(
                        ecs::ComponentDecodeFailure{ecs::EComponentDecodeError::UNRESOLVED_REFERENCE}
                    );
                return entity;
            }
        };
        auto decoded = schema.decode_value(value.version, value.bytes, resolver, schema.code_lifetime);
        if (!decoded)
            return rejected(ESceneEditError::CODEC);
        std::move(*decoded).installInto(destination, entity);
        return {};
    }
    SceneEditResult<SceneComponentData> encodeComponent(
        const ecs::Registry& registry,
        ecs::Entity entity,
        const ecs::ComponentSchema& schema,
        const ecs::WorldEntityMap& identities,
        SceneBudget& budget
    )
    {
        auto captured = schema.capture(registry, entity, schema.code_lifetime);
        if (!captured)
            return rejected(ESceneEditError::CODEC);
        auto encoded = captured->encode(identities, budget.remaining());
        if (!encoded)
            return rejected(ESceneEditError::CODEC);
        if (!budget.take(sizeof(SceneComponentData) + schema.id.name.size() + encoded->size()))
            return rejected(ESceneEditError::BUDGET);
        return SceneComponentData{schema.id, schema.version, std::move(*encoded)};
    }

    namespace
    {
        SceneEditResult<void> checkRef(SceneObjectRef ref, sessions::ContentStamp expected)
        {
            if (ref.session != expected.session || ref.history != expected.state.history)
                return rejected(ESceneEditError::STALE_OBJECT, ref.object);
            return {};
        }
        bool structural(const VSceneEdit& value)
        {
            return !std::holds_alternative<SceneSetField>(value) &&
                   !std::holds_alternative<SceneSetConfiguration>(value);
        }

        bool sameStorage(const world::WorldDescription& a, const world::WorldDescription& b)
        {
            const bool same_identity = a.bundleId() == b.bundleId() && a.generation() == b.generation();
            const bool same_partitioner = a.partitioner().id == b.partitioner().id &&
                                          a.partitioner().version == b.partitioner().version &&
                                          a.partitionCount() == b.partitionCount();
            const bool same_table = std::ranges::equal(
                a.partitionTable().pages(),
                b.partitionTable().pages(),
                [](const auto& x, const auto& y) {
                    return x.first == y.first && x.count == y.count && x.chunk == y.chunk;
                }
            );
            const bool same_indexes =
                std::ranges::equal(a.partitionIndexes(), b.partitionIndexes(), [](const auto& x, const auto& y) {
                    return x.type == y.type && x.version == y.version && x.root == y.root;
                });
            const bool same_volumes =
                std::ranges::equal(a.storageVolumes(), b.storageVolumes(), [](const auto& x, const auto& y) {
                    return x.member_name == y.member_name && x.format_version == y.format_version &&
                           x.chunk_count == y.chunk_count && x.file_size == y.file_size;
                });
            return same_identity && same_partitioner && same_table && same_indexes && same_volumes;
        }

        template <class Asset> SceneEditResult<bool> sameAsset(const Asset& a, const Asset& b, SceneBudget& budget)
        {
            auto before = asset::TAssetSerDeser<Asset>::encode(a, asset::AssetEncodeLimits{budget.remaining()});
            auto after = asset::TAssetSerDeser<Asset>::encode(b, asset::AssetEncodeLimits{budget.remaining()});
            if (!before || !after)
                return rejected(ESceneEditError::CODEC);
            return *before == *after;
        }

        SceneEditResult<ScenePreparedInput> prepareInput(SceneSessionAccess::Data& owner, const SceneEditBatch& batch)
        {
            const auto& source = SceneSourceAccess::data(owner.source);
            SceneBudget budget{owner.limits.history.max_staging_bytes};
            ScenePreparedInput result;
            result.change.structure = std::ranges::any_of(batch.edits, structural);
            std::vector<SceneObjectData> before;
            std::vector<SceneObjectData> after;
            const bool changes_content = std::ranges::any_of(batch.edits, [](const auto& edit) {
                return !std::holds_alternative<SceneSetConfiguration>(edit);
            });
            if (changes_content && !source.configuration.world->data().partitionIndexes().empty())
                return rejected(ESceneEditError::INDEX_REBUILD_REQUIRED);
            if (result.change.structure)
            {
                auto content = SceneSourceAccess::objects(source, budget);
                if (!content)
                    return lux::cxx::unexpected(content.error());
                before = std::move(*content);
                for (const auto& object : before)
                    if (!budget.take(objectBytes(object)))
                        return rejected(ESceneEditError::BUDGET, object.id);
                after = before;
            }

            // Identity planning is CPU-only. References in field codecs are resolved against the complete final set.
            ecs::Registry scratch;
            ecs::WorldEntityMap candidate_identities;
            std::unordered_map<world::WorldObjectId, ecs::Entity, world::WorldObjectIdHash> field_entities;
            const auto& identities = result.change.structure ? candidate_identities : source.identities;
            if (result.change.structure)
                for (const auto& object : source.objects)
                    if (!candidate_identities.bind(object.id, scratch.create()))
                        std::terminate();
            for (const auto& intent : batch.edits)
            {
                if (const auto* create = std::get_if<SceneCreateObject>(&intent))
                {
                    if (!create->value.id.valid())
                        return rejected(ESceneEditError::INVALID_OBJECT);
                    if (!candidate_identities.bind(create->value.id, scratch.create()))
                        return rejected(ESceneEditError::DUPLICATE_OBJECT, create->value.id);
                    if (!budget.take(objectBytes(create->value)))
                        return rejected(ESceneEditError::BUDGET, create->value.id);
                    after.push_back(create->value);
                }
            }
            std::unordered_set<world::WorldObjectId, world::WorldObjectIdHash> removed;
            for (const auto& intent : batch.edits)
            {
                if (const auto* erase = std::get_if<SceneEraseObject>(&intent))
                {
                    if (auto checked = checkRef(erase->target, batch.expected); !checked)
                        return lux::cxx::unexpected(checked.error());
                    const auto found = std::ranges::find(after, erase->target.object, &SceneObjectData::id);
                    if (found == after.end())
                        return rejected(ESceneEditError::INVALID_OBJECT, erase->target.object);
                    removed.insert(erase->target.object);
                    after.erase(found);
                    candidate_identities.unbind(identities.entity(erase->target.object));
                }
            }
            if (!removed.empty())
            {
                for (const auto& object : source.objects)
                    if (!object.components.empty())
                        return rejected(ESceneEditError::UNKNOWN_REFERENCE, object.id);
            }

            // Decode only components touched by a field operation; several fields share the same scratch value.
            for (const auto& intent : batch.edits)
            {
                const auto apply = std::visit(
                    [&](const auto& edit) -> SceneEditResult<void> {
                        using Edit = std::remove_cvref_t<decltype(edit)>;
                        if constexpr (std::same_as<Edit, SceneCreateObject> || std::same_as<Edit, SceneEraseObject>)
                            return {};
                        else if constexpr (std::same_as<Edit, SceneSetConfiguration>)
                        {
                            auto frozen = SceneSourceAccess::freezeConfiguration(edit.value, budget);
                            if (!frozen)
                                return lux::cxx::unexpected(frozen.error());
                            // Partition/index changes need a builder capable of preserving unknown associations.
                            const auto& a = source.configuration.world->data();
                            const auto& b = frozen->world->data();
                            if (!sameStorage(a, b))
                                return rejected(ESceneEditError::INDEX_REBUILD_REQUIRED);
                            const bool is_identity_change =
                                source.configuration.scene->id() != frozen->scene->id() ||
                                source.configuration.world->id() != frozen->world->id() ||
                                source.configuration.simulation->id() != frozen->simulation->id();
                            if (is_identity_change)
                                return rejected(ESceneEditError::INVALID_SOURCE);
                            for (const auto& schema : a.schemas())
                                if (std::ranges::find(b.schemas(), schema) == b.schemas().end())
                                    return rejected(ESceneEditError::MISSING_SCHEMA);
                            auto same_scene = sameAsset(*source.configuration.scene, *frozen->scene, budget);
                            if (!same_scene)
                                return lux::cxx::unexpected(same_scene.error());
                            auto same_world = sameAsset(*source.configuration.world, *frozen->world, budget);
                            if (!same_world)
                                return lux::cxx::unexpected(same_world.error());
                            auto same_simulation =
                                sameAsset(*source.configuration.simulation, *frozen->simulation, budget);
                            if (!same_simulation)
                                return lux::cxx::unexpected(same_simulation.error());
                            if (*same_scene && *same_world && *same_simulation)
                            {
                                result.configuration.reset();
                                result.change.configuration = false;
                                return {};
                            }
                            result.configuration = std::move(*frozen);
                            result.change.configuration = true;
                            return {};
                        }
                        else
                        {
                            const auto target = [&] {
                                if constexpr (std::same_as<Edit, SceneSetField> ||
                                              std::same_as<Edit, SceneRemoveComponent>)
                                    return edit.target.target;
                                else
                                    return edit.target;
                            }();
                            if (auto checked = checkRef(target, batch.expected); !checked)
                                return lux::cxx::unexpected(checked.error());
                            const auto entity = identities.entity(target.object);
                            if (entity == ecs::NullEntity)
                                return rejected(ESceneEditError::INVALID_OBJECT, target.object);
                            result.change.objects.push_back(target.object);
                            if constexpr (std::same_as<Edit, SceneSetField>)
                            {
                                if (!edit.value || edit.target.field.empty())
                                    return rejected(ESceneEditError::INVALID_FIELD);
                                const auto* schema = source.schemas.find(edit.target.component);
                                const bool is_wrong_type = !schema || schema->cpp_type != edit.value->type();
                                if (is_wrong_type)
                                    return rejected(ESceneEditError::MISSING_SCHEMA, target.object);
                                if (schema->id.name == "lux.ecs.Parent" || !schema->decode_value || !schema->capture)
                                    return rejected(ESceneEditError::INVALID_FIELD, target.object);
                                if (!budget.take(edit.value->bytes()))
                                    return rejected(ESceneEditError::BUDGET, target.object);
                                const auto scratch_entity = [&] {
                                    if (result.change.structure)
                                        return entity;
                                    auto [found, inserted] = field_entities.try_emplace(target.object, ecs::NullEntity);
                                    if (inserted)
                                        found->second = scratch.create();
                                    return found->second;
                                }();
                                auto delta = std::ranges::find_if(result.fields, [&](const auto& value) {
                                    return value.object == target.object && value.before.schema == schema->id;
                                });
                                if (delta == result.fields.end())
                                {
                                    SceneEditResult<SceneComponentData> original =
                                        rejected(ESceneEditError::INVALID_COMPONENT);
                                    if (result.change.structure)
                                    {
                                        const auto row = std::ranges::find(after, target.object, &SceneObjectData::id);
                                        const auto value =
                                            std::ranges::find(row->components, schema->id, &SceneComponentData::schema);
                                        if (value != row->components.end())
                                            original = *value;
                                    }
                                    else
                                        original =
                                            SceneSourceAccess::component(source, target.object, schema->id, budget);
                                    if (!original)
                                        return lux::cxx::unexpected(original.error());
                                    if (auto decoded =
                                            decodeInto(scratch, scratch_entity, *original, *schema, identities);
                                        !decoded)
                                        return decoded;
                                    result.fields.push_back(
                                        {target.object, std::move(*original), {}, edit.code, edit.value->swap()}
                                    );
                                }
                                if (!edit.value->apply(
                                        const_cast<void*>(schema->operations.get(scratch, scratch_entity)),
                                        edit.target.field
                                    ))
                                    return rejected(ESceneEditError::INVALID_FIELD, target.object);
                                return {};
                            }
                            else
                            {
                                auto row = std::ranges::find(after, target.object, &SceneObjectData::id);
                                if (row == after.end())
                                    return rejected(ESceneEditError::INVALID_OBJECT, target.object);
                                if constexpr (std::same_as<Edit, SceneAddComponent>)
                                {
                                    if (!source.schemas.find(edit.value.schema))
                                        return rejected(ESceneEditError::MISSING_SCHEMA);
                                    if (std::ranges::find(
                                            row->components,
                                            edit.value.schema,
                                            &SceneComponentData::schema
                                        ) != row->components.end())
                                        return rejected(ESceneEditError::INVALID_COMPONENT, target.object);
                                    row->components.push_back(edit.value);
                                }
                                else if constexpr (std::same_as<Edit, SceneRemoveComponent>)
                                {
                                    auto value = std::ranges::find(
                                        row->components,
                                        edit.target.component,
                                        &SceneComponentData::schema
                                    );
                                    if (value == row->components.end())
                                        return rejected(ESceneEditError::INVALID_COMPONENT);
                                    if (!source.schemas.find(value->schema))
                                        return rejected(ESceneEditError::UNKNOWN_REFERENCE);
                                    row->components.erase(value);
                                }
                                else if constexpr (std::same_as<Edit, SceneReparentObject>)
                                {
                                    const auto* schema = source.schemas.find(lux::cxx::typeToken<ecs::Parent>());
                                    if (!schema || !schema->capture_value)
                                        return rejected(ESceneEditError::MISSING_SCHEMA);
                                    const auto parent = identities.entity(edit.parent);
                                    if (edit.parent.valid() && parent == ecs::NullEntity)
                                        return rejected(ESceneEditError::UNRESOLVED_REFERENCE);
                                    const ecs::Parent value{parent};
                                    auto encoded = schema->capture_value(&value, schema->code_lifetime)
                                                       .encode(identities, budget.remaining());
                                    if (!encoded)
                                        return rejected(ESceneEditError::CODEC);
                                    SceneComponentData data{schema->id, schema->version, std::move(*encoded)};
                                    auto current =
                                        std::ranges::find(row->components, schema->id, &SceneComponentData::schema);
                                    if (current == row->components.end())
                                        row->components.push_back(std::move(data));
                                    else
                                        *current = std::move(data);
                                }
                                return {};
                            }
                        }
                    },
                    intent
                );
                if (!apply)
                    return lux::cxx::unexpected(apply.error());
            }
            for (auto& field : result.fields)
            {
                const auto* schema = source.schemas.find(field.before.schema);
                const auto entity =
                    result.change.structure ? identities.entity(field.object) : field_entities.at(field.object);
                auto encoded = encodeComponent(scratch, entity, *schema, identities, budget);
                if (!encoded)
                    return lux::cxx::unexpected(encoded.error());
                field.after = std::move(*encoded);
                if (result.change.structure)
                {
                    auto row = std::ranges::find(after, field.object, &SceneObjectData::id);
                    if (row == after.end())
                        return rejected(ESceneEditError::INVALID_OBJECT, field.object);
                    auto value = std::ranges::find(row->components, schema->id, &SceneComponentData::schema);
                    if (value == row->components.end())
                        return rejected(ESceneEditError::INVALID_COMPONENT, field.object);
                    *value = field.after;
                }
            }
            std::erase_if(result.fields, [](const auto& field) { return field.before == field.after; });
            if (result.change.structure)
            {
                for (const auto& object : before)
                {
                    const auto found = std::ranges::find(after, object.id, &SceneObjectData::id);
                    if (found == after.end())
                        result.objects.push_back({object.id, object, {}});
                    else if (object != *found)
                        result.objects.push_back({object.id, object, *found});
                }
                for (const auto& object : after)
                    if (std::ranges::find(before, object.id, &SceneObjectData::id) == before.end())
                        result.objects.push_back({object.id, {}, object});
                result.fields.clear();
                result.initial_objects = std::move(after);
                result.change.structure = !result.objects.empty();
            }
            result.change.objects.clear();
            for (const auto& object : result.objects)
            {
                result.change.objects.push_back(object.id);
                if (object.before)
                    result.retained_bytes = addFieldBytes(result.retained_bytes, objectBytes(*object.before));
                if (object.after)
                    result.retained_bytes = addFieldBytes(result.retained_bytes, objectBytes(*object.after));
            }
            for (const auto& field : result.fields)
            {
                result.change.objects.push_back(field.object);
                result.retained_bytes = addFieldBytes(
                    result.retained_bytes,
                    sizeof(field) + field.before.bytes.size() + field.after.bytes.size() +
                        field.before.schema.name.size() * 2
                );
            }
            if (result.configuration)
                result.retained_bytes = addFieldBytes(result.retained_bytes, budget.used());
            return result;
        }

        class SceneObjectPlan final : public editing::PreparedEdit
        {
        public:
            SceneObjectPlan(const SceneEditMemento& edit, SceneSource candidate)
                : edit_(edit), candidate_(std::move(candidate)), change_(edit.input.change)
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return editing::EEditEffect::CHANGE;
            }

        private:
            void apply() noexcept override
            {
                SceneSourceAccess::swap(edit_.owner.source, candidate_);
            }
            void publish(const editing::CommitInfo&) noexcept override
            {
                std::vector<SceneObjectData>{}.swap(edit_.initial_objects);
                edit_.owner.publish(std::move(change_));
            }
            const SceneEditMemento& edit_;
            SceneSource candidate_;
            SceneChangeRecord change_;
        };
    }

    class SceneObjectEdit final : public editing::EditOperation
    {
    public:
        SceneObjectEdit(
            SceneSessionAccess::Data& owner,
            sessions::ContentStamp expected,
            std::string label,
            ScenePreparedInput input
        )
            : edit_{
                  owner,
                  expected,
                  std::move(label),
                  std::move(input),
                  SceneSourceAccess::data(owner.source).configuration,
                  {}
              }
        {
            edit_.initial_objects = std::move(edit_.input.initial_objects);
        }
        editing::HistoryId historyId() const noexcept override
        {
            return edit_.expected.state.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return edit_.expected.state;
        }
        std::string_view label() const noexcept override
        {
            return edit_.label;
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return addFieldBytes(sizeof(*this) + edit_.label.capacity(), edit_.input.retained_bytes);
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            if (!edit_.input.change.structure)
            {
                if (edit_.input.fields.empty())
                    return prepareSceneConfiguration(edit_, context, budget);
                return prepareSceneFields(edit_, context, budget);
            }
            SceneBudget preparation{budget.remaining()};
            const auto& source = SceneSourceAccess::data(edit_.owner.source);
            std::vector<SceneObjectData> records;
            std::span<const SceneObjectData> input;
            if (context.kind == editing::EApplyKind::EXECUTE)
                input = edit_.initial_objects;
            else
            {
                auto current = SceneSourceAccess::objects(source, preparation);
                if (!current)
                    return lux::cxx::unexpected(editFailure(current.error()));
                records = std::move(*current);
                for (const auto& delta : edit_.input.objects)
                {
                    const auto& to = context.direction == editing::EDirection::FORWARD ? delta.after : delta.before;
                    auto found = std::ranges::find(records, delta.id, &SceneObjectData::id);
                    if (!to)
                    {
                        if (found != records.end())
                            records.erase(found);
                    }
                    else if (found == records.end())
                        records.push_back(*to);
                    else
                        *found = *to;
                }
                input = records;
            }
            const auto configuration = edit_.input.configuration ? (context.direction == editing::EDirection::FORWARD
                                                                        ? *edit_.input.configuration
                                                                        : edit_.before_configuration)
                                                                 : source.configuration;
            auto candidate = SceneSourceAccess::build(configuration, source.schemas, input, preparation);
            if (!candidate)
                return lux::cxx::unexpected(editFailure(candidate.error()));
            auto& data = SceneSourceAccess::data(*candidate);
            // Already-frozen immutable bytes can share ownership with a candidate.
            data.volumes = source.volumes;
            data.package = source.package;
            auto charged = budget.reserve(preparation.used() + sizeof(SceneObjectPlan) + edit_.input.change.bytes());
            if (!charged)
                return lux::cxx::unexpected(charged.error());
            return editing::PreparedEditPtr(std::make_unique<SceneObjectPlan>(edit_, std::move(*candidate)));
        }

    private:
        SceneEditMemento edit_;
    };

    SceneEditResult<editing::EditOperationPtr> makeSceneEdit(SceneSessionAccess::Data& owner, SceneEditBatch batch)
    {
        auto input = prepareInput(owner, batch);
        if (!input)
            return lux::cxx::unexpected(input.error());
        return editing::EditOperationPtr(
            std::make_unique<SceneObjectEdit>(owner, batch.expected, std::move(batch.label), std::move(*input))
        );
    }
}
