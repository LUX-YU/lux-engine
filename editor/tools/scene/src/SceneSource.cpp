#include <lux/engine/editor/scene/detail/SceneSource.hpp>

namespace lux::editor::scene::detail
{
    EditorResult<lux::scene::ScenePackage> copySceneSource(
        const lux::scene::ScenePackage& source,
        asset::AssetId id,
        std::stop_token stop
    ) noexcept
    {
        const bool unknown_roots = !source.scene->auxiliaryPayloads().empty() ||
                                   !source.world->auxiliaryPayloads().empty() ||
                                   !source.simulation->auxiliaryPayloads().empty();
        const bool unsupported_structure = !source.world->data().partitionIndexes().empty() ||
                                           source.package.entries.size() != source.volumes.size() + 3;
        if (unknown_roots || unsupported_structure)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::INVALID_STATE,
                "scene.save-as.extensions",
                0,
                "The package contains extensions or indexes whose identity references cannot be rewritten safely"
            });
        auto result = lux::scene::assembleScenePackage(
            id,
            source.world->data().name(),
            source.world->data().schemas(),
            source.world->data().partitioner(),
            source.partitions,
            source.simulation->sharedData(),
            source.scene->data(),
            stop
        );
        if (!result)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "scene.save-as", 0, {}, result.error()}
            );
        return std::move(*result);
    }
}
