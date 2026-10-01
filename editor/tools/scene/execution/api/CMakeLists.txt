add_interface_component(COMPONENT_NAME scene_execution_api NAMESPACE lux::engine::editor)
component_include_directories(scene_execution_api
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_execution_api INTERFACE lux::engine::editor::scene_model
    lux::engine::scene::scene_composition lux::engine::function::render_client)
component_add_transitive_commands(scene_execution_api
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)")
lux_classify_target(TARGET scene_execution_api LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

