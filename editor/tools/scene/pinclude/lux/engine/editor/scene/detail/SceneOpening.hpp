#pragma once
namespace lux::project
{
    struct SceneRegistrations;
}
#include <lux/engine/scene/SceneCapture.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/scene/RenderResources.hpp>
namespace lux::editor
{
    class ProjectStorage;
}
namespace lux::editor::scene
{
    namespace detail
    {
        [[nodiscard]] EditorResult<lux::scene::SceneInstanceLease> instantiateScenePackage(
            lux::scene::SceneRuntime&,
            const lux::scene::ScenePackage&,
            const lux::project::SceneRegistrations&,
            process::ExecutionRuntime&,
            lux::render::RenderRuntime&,
            lux::scene::RenderResources&,
            lux::scene::RenderAssetInput
        );
    } // namespace detail

}
