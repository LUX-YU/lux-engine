#pragma once
#include <lux/engine/scene/SceneCapture.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/scene/RenderResources.hpp>
namespace lux::editor
{
    class ProjectStorage;
    struct SceneRegistrations;
}
namespace lux::editor::scene
{
    namespace detail
    {
        [[nodiscard]] EditorResult<lux::scene::SceneInstanceId> instantiateScenePackage(
            lux::scene::SceneRuntime&,
            const lux::scene::ScenePackage&,
            const SceneRegistrations&,
            process::TaskScope&,
            lux::render::RenderRuntime&,
            lux::scene::RenderResources&,
            lux::scene::RenderAssetInput,

            bool open_all_partitions,
            lux::scene::FixedStepClock clock = {}
        );
    } // namespace detail

}
