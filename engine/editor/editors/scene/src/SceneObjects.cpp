#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <algorithm>
#include <lux/engine/editor/scene/detail/SceneObjects.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>

namespace lux::editor::scene::detail
{
static auto structureFailure(ESceneStructureError code, std::string_view message)
{
    return lux::cxx::unexpected(
        editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, static_cast<std::uint64_t>(code), message));
}

editing::EditResult<ObjectContent> SceneObjects::captureObject(
    SceneObjectRow row, const lux::simulation::ecs::Registry &registry,
    const lux::simulation::ecs::WorldEntityMap &mapping) const
{
    namespace ecs = lux::simulation::ecs;
    const auto entity = resolve(row.object);
    ObjectContent result{
        {mapping.object(entity), mapping.object(row.parent.entity), std::move(row.label), row.partition}, {}};
    std::size_t retained{};
    for (const auto &schema : metadata.all())
    {
        if (schema.semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
            !schema.operations.has(registry, entity))
        {
            continue;
        }
        if (!schema.capture || !schema.decode_value)
        {
            return structureFailure(ESceneStructureError::MISSING_PROVIDER, schema.id.name);
        }
        auto capture = schema.capture(registry, entity, schema.code_lifetime);
        if (!capture)
        {
            return structureFailure(ESceneStructureError::CODEC_FAILURE, schema.id.name);
        }
        auto bytes = capture->encode(mapping, kSceneHistoryLimits.max_staging_bytes - retained);
        if (!bytes)
        {
            return structureFailure(ESceneStructureError::CODEC_FAILURE, schema.id.name);
        }
        retained += bytes->size();
        result.components.push_back({&schema, std::move(*bytes)});
    }
    return result;
}

bool SceneObjects::componentLess(const ComponentNotice &first, const ComponentNotice &second) noexcept
{
    if (first.object != second.object)
    {
        return first.object < second.object;
    }
    return first.component.hash() < second.component.hash();
}

std::vector<lux::scene::PartitionRetention> SceneObjects::retainTargets(lux::simulation::ecs::Entity entity,
                                                                        lux::cxx::TypeToken type) const
{
    std::vector<lux::simulation::ecs::Entity> entities{entity};
    const auto *schema = metadata.find(type);
    if (schema && schema->operations.has(registry, entity) && schema->visit_references)
    {
        static_cast<void>(schema->visit_references(
            registry, entity, {&entities, +[](void *state, auto referenced) noexcept {
                                   static_cast<std::vector<lux::simulation::ecs::Entity> *>(state)->push_back(
                                       referenced);
                               }}));
    }
    std::vector<lux::partition::PartitionOrdinal> partitions;
    std::vector<lux::scene::PartitionRetention> retained;
    for (const auto target : entities)
    {
        lux::partition::PartitionOrdinal partition;
        if (loading.partitionOf(target, partition) && std::ranges::find(partitions, partition) == partitions.end())
        {
            auto token = loading.retain(partition);
            assert(token);
            retained.push_back(std::move(*token));
            partitions.push_back(partition);
        }
    }
    return retained;
}

SceneObjects::SceneObjects(lux::scene::SceneInstance &scene, const NativeScene &content,
                           const lux::simulation::ecs::ComponentSchemaSet &meta)
    : registry(scene.registry()), source(content), metadata(meta),
      loading(*scene.findSceneSystem<lux::scene::WorldLoadingSystem>()), identities(loading.identities()),
      instance(scene.id())
{
    selection.object = reference(lux::simulation::ecs::NullEntity);
    rows.reserve(identities.size());
    for (const auto &[id, entity] : identities.entries())
    {
        SceneObjectRow row{reference(entity), reference(lux::simulation::ecs::NullEntity),
                           "Object " + std::to_string(rows.size() + 1)};
        if (const auto *parent = registry.try_get<lux::simulation::ecs::Parent>(entity))
        {
            row.parent = reference(parent->entity);
        }
        rows.push_back(std::move(row));
    }
    std::ranges::sort(rows, std::less<SceneEntityRef>{}, &SceneObjectRow::object);
    for (const auto &partition : source.partitions)
    {
        for (std::size_t index{}; index < partition->objectCount(); ++index)
        {
            const auto object = partition->objectAt(index).id();
            auto row = std::ranges::lower_bound(rows, authorReference(object), std::less<SceneEntityRef>{},
                                                &SceneObjectRow::object);
            if (row != rows.end() && row->object == authorReference(object))
            {
                row->partition = partition->partition();
            }
        }
    }
    for (const auto &row : rows)
    {
        const auto entity = row.object.entity;
        for (const auto &schema : metadata.all())
        {
            if (schema.operations.has(registry, entity))
            {
                component_versions.push_back({row.object, schema.cpp_type, {}, false});
            }
        }
    }
    std::ranges::sort(component_versions, componentLess);
}
} // namespace lux::editor::scene::detail
