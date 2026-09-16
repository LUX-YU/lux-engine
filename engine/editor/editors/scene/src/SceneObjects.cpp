#include <algorithm>
#include <lux/engine/editor/scene/detail/SceneObjects.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>

namespace lux::editor::scene::detail
{
    static auto structureFailure(ESceneStructureError code, std::string_view message)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                             static_cast<std::uint64_t>(code), message));
    }

    editing::EditResult<ObjectContent> SceneObjects::captureObject(
        SceneObjectRow row, const lux::simulation::ecs::Registry &registry,
        const lux::simulation::ecs::WorldEntityMap &mapping) const
    {
        namespace ecs = lux::simulation::ecs;
        ObjectContent result{std::move(row), {}};
        const auto entity = mapping.entity(result.row.object);
        std::size_t retained{};
        for (const auto &schema : metadata.components().all())
        {
            if (schema.semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                !schema.operations.has(registry, entity))
            {
                continue;
            }
            const bool declared =
                std::ranges::find(source.world->data().schemas(), schema.id.name,
                                  &lux::world::WorldDataSchemaId::name) != source.world->data().schemas().end();
            if (!declared || !schema.capture || !schema.decode_value)
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
            return lux::world::WorldObjectIdLess{}(first.object, second.object);
        }
        return first.component.hash() < second.component.hash();
    }

    SceneObjects::SceneObjects(lux::simulation::ecs::Registry &storage, const NativeScene &content,
                               const lux::scene::SceneMetaManager &meta, lux::simulation::ecs::WorldEntityMap mapping)
        : registry(storage), source(content), metadata(meta), identities(std::move(mapping))
    {
        rows.reserve(identities.size());
        for (const auto &[id, entity] : identities.entries())
        {
            SceneObjectRow row{id, {}, "Object " + std::to_string(rows.size() + 1)};
            if (const auto *parent = registry.try_get<lux::simulation::ecs::Parent>(entity))
            {
                row.parent = identities.object(parent->entity);
            }
            rows.push_back(std::move(row));
        }
        std::ranges::sort(rows, lux::world::WorldObjectIdLess{}, &SceneObjectRow::object);
        for (const auto &partition : source.partitions)
        {
            for (std::size_t index{}; index < partition.objectCount(); ++index)
            {
                const auto object = partition.objectAt(index).id();
                auto row =
                    std::ranges::lower_bound(rows, object, lux::world::WorldObjectIdLess{}, &SceneObjectRow::object);
                row->partition = partition.partition();
            }
        }
        for (const auto &row : rows)
        {
            const auto entity = identities.entity(row.object);
            for (const auto &schema : metadata.components().all())
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
