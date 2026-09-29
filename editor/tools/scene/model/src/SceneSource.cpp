#include "SceneSourceData.hpp"
#include <lux/engine/editor/sessions/SessionState.hpp>
#include <lux/engine/editor/scene/FieldValue.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>

#include <lux/engine/simulation/ecs/Parent.hpp>
#include <algorithm>
#include <unordered_set>

namespace lux::editor::scene
{
    namespace ecs = simulation::ecs;
    namespace detail
    {
        namespace
        {
            lux::cxx::SharedBytes<> own(std::vector<std::byte> bytes)
            {
                auto storage = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
                return lux::cxx::SharedBytes<>::fromOwner(storage, *storage);
            }
            template <class Asset>
            SceneEditResult<std::shared_ptr<const Asset>> freezeAsset(
                const std::shared_ptr<const Asset>& value,
                SceneBudget& budget
            )
            {
                if (!value)
                    return rejected(ESceneEditError::INVALID_SOURCE);
                auto encoded =
                    asset::TAssetSerDeser<Asset>::encode(*value, asset::AssetEncodeLimits{budget.remaining()});
                if (!encoded)
                    return rejected(ESceneEditError::CODEC);
                const auto bytes = encoded->size() + sizeof(Asset);
                if (!budget.take(bytes))
                    return rejected(ESceneEditError::BUDGET);
                auto decoded = asset::TAssetSerDeser<Asset>::decode(
                    value->id(),
                    own(std::move(*encoded)),
                    asset::AssetDecodeLimits{bytes, budget.remaining(), 1024}
                );
                if (!decoded)
                    return rejected(ESceneEditError::CODEC);
                if (!budget.take((*decoded)->data().retainedBytes()))
                    return rejected(ESceneEditError::BUDGET);
                return std::move(*decoded);
            }
        }

        std::size_t objectBytes(const SceneObjectData& object) noexcept
        {
            std::size_t result = sizeof(object);
            for (const auto& value : object.components)
                result = addFieldBytes(result, sizeof(value) + value.schema.name.size() + value.bytes.size());
            return result;
        }

        SceneEditResult<SceneConfiguration> SceneSourceAccess::freezeConfiguration(
            const SceneConfiguration& value,
            SceneBudget& budget
        )
        {
            const bool is_incomplete = !value.scene || !value.world || !value.simulation;
            if (is_incomplete)
                return rejected(ESceneEditError::INVALID_SOURCE);
            const bool is_wrong_binding = value.scene->data().world() != value.world->id() ||
                                          value.scene->data().simulation() != value.simulation->id();
            if (is_wrong_binding)
                return rejected(ESceneEditError::INVALID_SOURCE);
            auto scene = freezeAsset(value.scene, budget);
            if (!scene)
                return lux::cxx::unexpected(scene.error());
            auto world = freezeAsset(value.world, budget);
            if (!world)
                return lux::cxx::unexpected(world.error());
            auto simulation = freezeAsset(value.simulation, budget);
            if (!simulation)
                return lux::cxx::unexpected(simulation.error());
            return SceneConfiguration{std::move(*scene), std::move(*world), std::move(*simulation)};
        }

        SceneEditResult<SceneComponentData> SceneSourceAccess::component(
            const Data& source,
            world::WorldObjectId object,
            const ecs::ComponentSchemaId& id,
            SceneBudget& budget
        )
        {
            const auto entity = source.identities.entity(object);
            if (entity == ecs::NullEntity)
                return rejected(ESceneEditError::INVALID_OBJECT, object);
            const auto* schema = source.schemas.find(id);
            if (schema && schema->operations.has(source.registry, entity))
            {
                if (!schema->capture)
                    return rejected(ESceneEditError::INVALID_COMPONENT, object);
                auto value = schema->capture(source.registry, entity, schema->code_lifetime);
                if (!value)
                    return rejected(ESceneEditError::CODEC, object);
                auto encoded = value->encode(source.identities, budget.remaining());
                if (!encoded)
                    return rejected(ESceneEditError::CODEC, object);
                if (!budget.take(sizeof(SceneComponentData) + id.name.size() + encoded->size()))
                    return rejected(ESceneEditError::BUDGET, object);
                return SceneComponentData{id, schema->version, std::move(*encoded)};
            }
            const auto found = std::ranges::find(source.objects, object, &SceneObjectData::id);
            const auto unknown = std::ranges::find(found->components, id, &SceneComponentData::schema);
            if (unknown == found->components.end())
                return rejected(ESceneEditError::INVALID_COMPONENT, object);
            if (!budget.take(sizeof(*unknown) + id.name.size() + unknown->bytes.size()))
                return rejected(ESceneEditError::BUDGET, object);
            return *unknown;
        }

        SceneEditResult<std::vector<SceneObjectData>> SceneSourceAccess::objects(
            const Data& source,
            SceneBudget& budget
        )
        {
            std::vector<SceneObjectData> result;
            if (!budget.take(source.objects.size() * sizeof(SceneObjectData)))
                return rejected(ESceneEditError::BUDGET);
            result.reserve(source.objects.size());
            for (const auto& object : source.objects)
            {
                SceneObjectData copy{object.id, object.partition, {}};
                for (const auto& unknown : object.components)
                {
                    if (!budget.take(sizeof(unknown) + unknown.schema.name.size() + unknown.bytes.size()))
                        return rejected(ESceneEditError::BUDGET, object.id);
                    copy.components.push_back(unknown);
                }
                const auto entity = source.identities.entity(object.id);
                for (const auto& schema : source.schemas.all())
                {
                    if (!schema.operations.has(source.registry, entity))
                        continue;
                    auto value = component(source, object.id, schema.id, budget);
                    if (!value)
                        return lux::cxx::unexpected(value.error());
                    copy.components.push_back(std::move(*value));
                }
                std::ranges::sort(copy.components, {}, [](const auto& v) { return v.schema.name; });
                result.push_back(std::move(copy));
            }
            return result;
        }

        SceneEditResult<void> SceneSourceAccess::validate(const Data& source)
        {
            for (const auto& object : source.objects)
            {
                const auto parent_of = [&](world::WorldObjectId id) {
                    const auto* parent = source.registry.try_get<ecs::Parent>(source.identities.entity(id));
                    return parent ? source.identities.object(parent->entity) : world::WorldObjectId{};
                };
                if (createsParentCycle(object.id, parent_of(object.id), source.objects.size(), parent_of))
                    return rejected(ESceneEditError::HIERARCHY_CYCLE, object.id);
            }
            return {};
        }

        SceneEditResult<SceneSource> SceneSourceAccess::build(
            SceneConfiguration configuration,
            ecs::ComponentSchemaSet schemas,
            std::span<const SceneObjectData> objects,
            SceneBudget& budget
        )
        {
            if (!configuration.scene || !configuration.world || !configuration.simulation)
                return rejected(ESceneEditError::INVALID_SOURCE);
            if (objects.size() > budget.remaining() / sizeof(SceneObjectData))
                return rejected(ESceneEditError::BUDGET);
            auto data = std::make_unique<Data>();
            data->schemas = std::move(schemas);
            data->configuration = std::move(configuration);
            data->objects.reserve(objects.size());
            std::unordered_set<world::WorldObjectId, world::WorldObjectIdHash> ids;
            std::vector<std::vector<lux::scene::WorldComponentInput>> components(objects.size());
            std::vector<lux::scene::WorldObjectInput> inputs;
            inputs.reserve(objects.size());
            for (std::size_t index{}; index < objects.size(); ++index)
            {
                const auto& object = objects[index];
                if (!object.id.valid())
                    return rejected(ESceneEditError::INVALID_OBJECT, object.id, index);
                if (!ids.insert(object.id).second)
                    return rejected(ESceneEditError::DUPLICATE_OBJECT, object.id, index);
                if (object.partition.value >= data->configuration.world->data().partitionCount())
                    return rejected(ESceneEditError::INVALID_PARTITION, object.id, index);
                if (!budget.take(objectBytes(object)))
                    return rejected(ESceneEditError::BUDGET, object.id, index);
                SceneObjectData record{object.id, object.partition, {}};
                std::unordered_set<std::string_view> seen;
                for (const auto& component : object.components)
                {
                    if (!seen.insert(component.schema.name).second)
                        return rejected(ESceneEditError::INVALID_COMPONENT, object.id, index);
                    const auto declared = data->configuration.world->data().schemas();
                    if (std::ranges::find(declared, component.schema.name, &world::WorldDataSchemaId::name) ==
                        declared.end())
                        return rejected(ESceneEditError::MISSING_SCHEMA, object.id, index);
                    const auto* schema = data->schemas.find(component.schema);
                    if (!schema)
                    {
                        record.components.push_back(component);
                        continue;
                    }
                    const bool is_unusable = schema->snapshot != ecs::EComponentSnapshotPolicy::COPY ||
                                             schema->semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                                             !schema->decode_value || !schema->capture;
                    if (is_unusable)
                        return rejected(ESceneEditError::INVALID_COMPONENT, object.id, index);
                    if (!budget.take(schema->operations.valueBytes()))
                        return rejected(ESceneEditError::BUDGET, object.id, index);
                    components[index].push_back({schema, component.version, component.bytes});
                }
                data->objects.push_back(std::move(record));
                inputs.push_back({object.id, components[index]});
            }
            auto prepared = lux::scene::WorldMaterializer::prepareObjects(data->registry, data->identities, inputs);
            if (!prepared)
                return rejected(ESceneEditError::CODEC, {}, prepared.error().object);
            // This installs only into the isolated candidate, never into the live author source.
            prepared->commit(data->registry, data->identities);
            if (auto checked = validate(*data); !checked)
                return lux::cxx::unexpected(checked.error());
            return SceneSource{std::move(data)};
        }

        SceneEditResult<void> SceneSourceAccess::copyOpaque(const Data& from, Data& to, SceneBudget& budget)
        {
            if (!budget.take(from.partition_ids.size() * sizeof(world::WorldPartitionId)))
                return rejected(ESceneEditError::BUDGET);
            to.partition_ids = from.partition_ids;
            if (!budget.take(from.package.mount_hint.size()))
                return rejected(ESceneEditError::BUDGET);
            to.package.mount_hint = from.package.mount_hint;
            for (const auto& bytes : from.volumes)
            {
                if (!budget.take(bytes.size() + sizeof(bytes)))
                    return rejected(ESceneEditError::BUDGET);
                to.volumes.push_back(lux::cxx::SharedBytes<>::copyOf(bytes.view()));
            }
            for (const auto& entry : from.package.entries)
            {
                if (!budget.take(entry.bytes.size() + sizeof(entry) + entry.metadata.vpath.size()))
                    return rejected(ESceneEditError::BUDGET);
                to.package.entries.push_back({entry.metadata, lux::cxx::SharedBytes<>::copyOf(entry.bytes.view())});
            }
            return {};
        }

        SceneEditResult<SceneSnapshot> SceneSourceAccess::capture(
            const SceneSource& source,
            sessions::ContentStamp stamp,
            SceneChangeCursor cursor,
            SnapshotBudget limit
        )
        {
            SceneBudget budget{limit.max_bytes};
            const auto& data = *source.data_;
            auto configuration = freezeConfiguration(data.configuration, budget);
            if (!configuration)
                return lux::cxx::unexpected(configuration.error());
            auto content = objects(data, budget);
            if (!content)
                return lux::cxx::unexpected(content.error());
            Data opaque;
            if (auto copied = copyOpaque(data, opaque, budget); !copied)
                return lux::cxx::unexpected(copied.error());
            SceneSnapshot result;
            result.schemas_ = data.schemas;
            result.content_ = stamp;
            result.cursor_ = cursor;
            result.configuration_ = std::move(*configuration);
            result.objects_ = std::move(*content);
            result.partition_ids_ = std::move(opaque.partition_ids);
            result.volumes_ = std::move(opaque.volumes);
            result.package_ = std::move(opaque.package);
            result.retained_bytes_ = budget.used();
            return result;
        }
    }

    SceneSource::SceneSource(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
    SceneSource::~SceneSource() noexcept = default;
    SceneSource::SceneSource(SceneSource&&) noexcept = default;
    SceneSource& SceneSource::operator=(SceneSource&&) noexcept = default;

    SceneEditResult<SceneSource> SceneSource::create(
        const lux::scene::ScenePackage& package,
        ecs::ComponentSchemaSet schemas,
        SnapshotBudget limit
    )
    {
        detail::SceneBudget budget{limit.max_bytes};
        auto configuration =
            detail::SceneSourceAccess::freezeConfiguration({package.scene, package.world, package.simulation}, budget);
        if (!configuration)
            return lux::cxx::unexpected(configuration.error());
        if (package.partitions.size() != package.world->data().partitionCount())
            return detail::rejected(ESceneEditError::INVALID_SOURCE);
        std::unordered_set<std::uint32_t> partitions;
        std::vector<SceneObjectData> objects;
        for (const auto& partition : package.partitions)
        {
            if (!partition)
                return detail::rejected(ESceneEditError::INVALID_SOURCE);
            const auto ordinal = partition->partition().value;
            if (ordinal >= package.world->data().partitionCount() || !partitions.insert(ordinal).second)
                return detail::rejected(ESceneEditError::INVALID_PARTITION);
            for (std::size_t index{}; index < partition->objectCount(); ++index)
            {
                const auto view = partition->objectAt(index);
                if (!budget.take(sizeof(SceneObjectData)))
                    return detail::rejected(ESceneEditError::BUDGET, view.id());
                SceneObjectData object{view.id(), partition->partition(), {}};
                for (std::size_t c{}; c < view.dataCount(); ++c)
                {
                    const auto ordinal = view.schemaOrdinalAt(c);
                    if (ordinal >= package.world->data().schemas().size())
                        return detail::rejected(ESceneEditError::INVALID_SOURCE);
                    const auto payload = view.payloadAt(c);
                    const auto& name = package.world->data().schemas()[ordinal].name;
                    if (!budget.take(sizeof(SceneComponentData) + name.size() + payload.size()))
                        return detail::rejected(ESceneEditError::BUDGET, view.id());
                    object.components.push_back(
                        {ecs::componentSchemaId(package.world->data().schemas()[ordinal].name),
                         view.schemaVersionAt(c),
                         {payload.begin(), payload.end()}}
                    );
                }
                objects.push_back(std::move(object));
            }
        }
        auto result = detail::SceneSourceAccess::build(std::move(*configuration), std::move(schemas), objects, budget);
        if (!result)
            return result;
        Data original;
        original.partition_ids.resize(package.world->data().partitionCount());
        for (const auto& partition : package.partitions)
            original.partition_ids[partition->partition().value] = partition->id();
        original.volumes = package.volumes;
        original.package = package.package;
        if (auto copied = detail::SceneSourceAccess::copyOpaque(original, *result->data_, budget); !copied)
            return lux::cxx::unexpected(copied.error());
        return result;
    }

    std::vector<SceneObjectRef> SceneReadView::objects() const
    {
        std::vector<SceneObjectRef> result;
        result.reserve(source_->data_->objects.size());
        for (const auto& object : source_->data_->objects)
            result.push_back({stamp_.session, stamp_.state.history, object.id});
        return result;
    }
    bool SceneReadView::contains(SceneObjectRef target) const noexcept
    {
        const bool is_same_source = target.session == stamp_.session && target.history == stamp_.state.history;
        return is_same_source && source_->data_->identities.entity(target.object) != ecs::NullEntity;
    }
    SceneEditResult<world::WorldObjectId> SceneReadView::parent(SceneObjectRef target) const noexcept
    {
        if (!contains(target))
            return detail::rejected(ESceneEditError::STALE_OBJECT, target.object);
        const auto& data = *source_->data_;
        const auto* value = data.registry.try_get<ecs::Parent>(data.identities.entity(target.object));
        return value ? data.identities.object(value->entity) : world::WorldObjectId{};
    }
    SceneEditResult<SceneComponentData> SceneReadView::component(
        SceneObjectRef target,
        const ecs::ComponentSchemaId& schema
    ) const
    {
        return gate_.withRead([&]() -> SceneEditResult<SceneComponentData> {
            if (!contains(target))
                return detail::rejected(ESceneEditError::STALE_OBJECT, target.object);
            detail::SceneBudget budget{(std::numeric_limits<std::size_t>::max)()};
            return detail::SceneSourceAccess::component(*source_->data_, target.object, schema, budget);
        });
    }
    const SceneConfiguration& SceneReadView::configuration() const noexcept
    {
        return source_->data_->configuration;
    }
}
