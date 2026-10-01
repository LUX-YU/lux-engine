add_component(COMPONENT_NAME editor_launcher NAMESPACE lux::engine::editor
    STATIC SOURCE_FILES src/ProjectCreationPane.cpp)
component_include_directories(editor_launcher
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/editor/activities/sinclude INSTALL_TIME include)
lux_classify_target(TARGET editor_launcher LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
target_link_libraries(editor_launcher PUBLIC lux::engine::editor::editor_ui
    lux::engine::editor::editor_storage PRIVATE lux::engine::editor::editor_launch lux::engine::scene::scene_asset)
component_add_transitive_commands(editor_launcher
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage)"
    "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)"
    "find_package(lux-engine-editor-launch REQUIRED COMPONENTS editor_launch)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-launcher VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_launcher)

add_executable(lux_launcher main.cpp)
lux_classify_target(TARGET lux_launcher LAYER EDITOR PRODUCT EDITOR ROLE EXECUTABLE)
target_link_libraries(lux_launcher PRIVATE editor_launcher lux::engine::engine_context_render
    lux::engine::platform::window lux::engine::platform::platform_process lux::engine::function::input)
install(TARGETS lux_launcher RUNTIME DESTINATION bin)
