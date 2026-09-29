#include <lux/engine/editor/scene/detail/SceneOpening.hpp>
#include <lux/engine/editor/metadata/SceneRegistrations.hpp>
#include <lux/engine/editor/scene/SceneProjection.hpp>
namespace lux::editor::scene
{
    EditorResult<lux::scene::SceneInstanceLease> detail::instantiateScenePackage(
        lux::scene::SceneRuntime& runtime,
        const lux::scene::ScenePackage& source,
        const SceneRegistrations& metadata,
        process::ExecutionRuntime& execution,
        render::RenderRuntime& renderer,
        lux::scene::RenderResources& resources,
        lux::scene::RenderAssetInput assets
    )
    {
        ProjectionEnvironment environment{
            metadata.components,
            metadata.simulation_systems,
            metadata.scene_systems,
            metadata.render_bindings,
            &renderer,
            &resources,
            std::move(assets)
        };
        auto result =
            instantiateAuthorProjection(runtime, source, environment, std::make_shared<process::TaskScope>(execution));
        if (!result)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "scene.create", 0, {}, result.error()}
            );
        return std::move(*result);
    }
}
