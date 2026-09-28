#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <algorithm>
#include <lux/engine/editor/scene/detail/SceneContent.hpp>

namespace lux::editor::scene::detail
{
    static auto structureFailure(ESceneStructureError code, std::string_view message)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(
            editing::EEditError::PRECONDITION_FAILED,
            static_cast<std::uint64_t>(code),
            message
        ));
    }

    editing::EditResult<ObjectContent> SceneContent::captureObject(lux::simulation::ecs::Entity entity) const
    {
        const auto& mapping = identities();
        const auto& registry = readRegistry();
        const auto& residency = registry.ctx().get<lux::scene::WorldResidency>();
        namespace ecs = lux::simulation::ecs;
        lux::partition::PartitionOrdinal partition;
        if (!registry.valid(entity) || !mapping.object(entity).valid() || !residency.partitionOf(entity, partition))
            return structureFailure(ESceneStructureError::INVALID_OBJECT, "The object has no persistent partition");
        ObjectContent result{mapping.object(entity), partition, {}};
        std::size_t retained{};
        for (const auto& schema : metadata.all())
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

    SceneContent::SceneContent(
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        const lux::scene::ScenePackage& content,
        const lux::simulation::ecs::ComponentSchemaSet& meta
    )
        : runtime(runtime), metadata(meta), instance(scene), source(content)
    {}
} // namespace lux::editor::scene::detail
