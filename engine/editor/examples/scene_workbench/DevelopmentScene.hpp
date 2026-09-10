#pragma once
#include <lux/engine/editor/application/Application.hpp>
namespace lux::editor::examples
{
    enum class EDemoBuildError : std::uint8_t
    {
        SCHEMA_BUILD_FAILURE,
        META_BUILD_FAILURE,
        WORLD_BUILD_FAILURE,
        SIMULATION_BUILD_FAILURE,
        SCENE_DESCRIPTION_BUILD_FAILURE,
        SCENE_BUILD_FAILURE,
        BINDING_BUILD_FAILURE,
        PANE_REGISTRATION_FAILURE,
        SELECTION_FAILURE,
        ALLOCATION_FAILURE,
    };

    [[nodiscard]] lux::cxx::expected<lux::scene::SceneMetaManager, EDemoBuildError>
    buildDevelopmentSceneMeta() noexcept;
    [[nodiscard]] sessions::SceneResult<sessions::SceneOpenInfo> openDevelopmentScene(
        sessions::SessionId, lux::object::ObjectDispatcherRef, rendering::EditorRenderer &,
        lux::process::asset_loading::AssetReadPort, std::shared_ptr<const lux::scene::SceneMetaManager>) noexcept;
    [[nodiscard]] sessions::SceneResult<sessions::SceneOpenInfo> openSharedShadowScene(
        sessions::SessionId, lux::object::ObjectDispatcherRef, rendering::EditorRenderer &,
        lux::process::asset_loading::AssetReadPort, std::shared_ptr<const lux::scene::SceneMetaManager>) noexcept;
    [[nodiscard]] sessions::SceneResult<sessions::SceneOpenInfo> openAlternateScene(
        sessions::SessionId, lux::object::ObjectDispatcherRef, rendering::EditorRenderer &,
        lux::process::asset_loading::AssetReadPort, std::shared_ptr<const lux::scene::SceneMetaManager>) noexcept;
    [[nodiscard]] sessions::SceneResult<sessions::SceneOpenInfo> openCoordinateScene(
        sessions::SessionId, lux::object::ObjectDispatcherRef, rendering::EditorRenderer &,
        lux::process::asset_loading::AssetReadPort, std::shared_ptr<const lux::scene::SceneMetaManager>,
        double) noexcept;
} // namespace lux::editor::examples
