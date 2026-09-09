#pragma once
// Non-installed diagnostic source mutations. No Registry or Session implementation escapes this boundary.
#include <lux/engine/editor/sessions/scene/SceneSession.hpp>
extern "C" LUX_EDITOR_SCENE_SESSION_PUBLIC void lux_er1_scene_allocation_fail_after(std::size_t) noexcept;
extern "C" LUX_EDITOR_SCENE_SESSION_PUBLIC std::size_t lux_er1_scene_allocation_disarm() noexcept;
namespace lux::editor::sessions::detail
{
    enum class ESceneTestMutation : std::uint8_t
    {
        ADJUST_VISUALS,
        ROTATE_MESH_SOURCES,
        REMOVE_VISUALS,
        CLEAR_SCENE
    };
    struct LUX_EDITOR_SCENE_SESSION_PUBLIC SceneTestAccess final
    {
        static SceneResult<void> mutateSource(SceneSession &, ESceneTestMutation) noexcept;
        static SceneResult<SceneEntityRef> recycleSelectedEntity(SceneSession &) noexcept;
        static SceneResult<void> replaceMeshSource(SceneSession &, SceneEntityRef, lux::asset::AssetId) noexcept;
    };
}
