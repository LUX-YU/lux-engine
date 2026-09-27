add_component(COMPONENT_NAME editor_settings NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/SettingPane.cpp src/PluginSettingsElement.cpp)
lux_classify_target(TARGET editor_settings LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_settings BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/editor/editing/sinclude INSTALL_TIME include)
target_link_libraries(editor_settings PUBLIC lux::engine::editor::editor_context lux::engine::editor::editor_ui)
component_add_transitive_commands(editor_settings
    "find_package(lux-engine-editor-context REQUIRED COMPONENTS editor_context)"
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-settings VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_settings)
