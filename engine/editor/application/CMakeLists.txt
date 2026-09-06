add_library(
    editor_application
    STATIC
    src/EditorApplication.cpp
    src/UiVulkanPresentation.cpp
)
target_include_directories(
    editor_application
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_link_libraries(
    editor_application
    PUBLIC
        lux::engine::editor::editor_context
        lux::engine::function::ui_vulkan_backend
        lux::engine::process::asset_loading
        lux::engine::function::render_vulkan
        lux::engine::platform::window
)
lux_classify_target(
    TARGET  editor_application
    LAYER   EDITOR
    PRODUCT EDITOR
    ROLE    COMPOSITION
)

add_executable(
    lux_editor
    app/main.cpp
    app/EditorBootstrap.cpp
)
target_include_directories(lux_editor PRIVATE app)
target_link_libraries(
    lux_editor
    PRIVATE
        editor_application
        lux::engine::editor::editor_inspector
        lux::engine::scene::scene_meta
)
lux_classify_target(
    TARGET  lux_editor
    LAYER   EDITOR
    PRODUCT EDITOR
    ROLE    COMPOSITION
)
set_target_properties(lux_editor PROPERTIES OUTPUT_NAME "lux_editor")

install(
    TARGETS lux_editor
    RUNTIME DESTINATION bin
    COMPONENT lux_editor
)
