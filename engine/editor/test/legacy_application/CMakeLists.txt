# Historical cost baseline only; never a production or installed entry.
add_library(
    editor_legacy_application
    STATIC
    src/EditorApplication.cpp
    src/UiVulkanPresentation.cpp
)
target_include_directories(
    editor_legacy_application
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_compile_definitions(editor_legacy_application PRIVATE
    LUX_EDITOR_PACKED_CONTENT=$<BOOL:${LUX_BUILD_PACKED_RENDER_CONTENT}>)
target_link_libraries(
    editor_legacy_application
    PUBLIC
        lux::engine::editor::editor_context
        lux::engine::function::ui_vulkan_backend
        lux::engine::process::asset_loading
        lux::engine::function::render_vulkan
        lux::engine::platform::window
        lux::engine::scene::scene_render
        lux::engine::editor::editor_scene
        lux::engine::function::render_features
)
lux_classify_target(
    TARGET  editor_legacy_application
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    COMPOSITION
)
