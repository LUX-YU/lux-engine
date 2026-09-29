#include <lux/engine/editor/scene/detail/SceneSource.hpp>

namespace lux::editor::scene::detail
{
    EditorResult<lux::scene::ScenePackage> copySceneSource(
        const lux::scene::ScenePackage& source,
        asset::AssetId id,
        std::stop_token stop
    ) noexcept
    {
        auto result = lux::scene::copyScenePackage(source, id, stop);
        if (!result && result.error().stage == "scene.save-as.extensions")
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::INVALID_STATE,
                "scene.save-as.extensions",
                0,
                "The package contains extensions or indexes whose identity references cannot be rewritten safely"
            });
        if (!result)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "scene.save-as", 0, {}, result.error()}
            );
        return std::move(*result);
    }
}
