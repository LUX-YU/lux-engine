add_component(COMPONENT_NAME scene_control_api_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneEditorCatalog.cpp)
component_include_directories(scene_control_api_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_control_api_legacy PUBLIC lux::engine::editor::editor_configuration_legacy
    lux::engine::editor::scene_model_legacy lux::engine::function::ui lux::engine::core::services)
component_add_transitive_commands(scene_control_api_legacy
    "find_package(lux-engine-editor-configuration REQUIRED COMPONENTS editor_configuration_legacy)"
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model_legacy)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET scene_control_api_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-controls VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_control_api_legacy)
