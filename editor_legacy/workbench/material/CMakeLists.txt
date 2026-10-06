find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME material_ui_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/MaterialNodeControls.cpp src/MaterialView.cpp)
component_include_directories(material_ui_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_ui_legacy PUBLIC lux::engine::editor::material_interaction_legacy lux::engine::editor::material_preview_legacy
    lux::engine::editor::editor_viewport_legacy lux::engine::editor::project_ui_legacy lux::engine::editor::editor_widgets_legacy lux::engine::editor::editor_composition_legacy PRIVATE imgui::core)
component_add_transitive_commands(material_ui_legacy
    "find_package(lux-engine-editor-material-interaction REQUIRED COMPONENTS material_interaction_legacy)"
    "find_package(lux-engine-editor-material-preview REQUIRED COMPONENTS material_preview_legacy)"
    "find_package(lux-engine-editor-viewport REQUIRED COMPONENTS editor_viewport_legacy)"
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui_legacy)"
    "find_package(lux-engine-editor-widgets REQUIRED COMPONENTS editor_widgets_legacy)"
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition_legacy)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET material_ui_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_ui_legacy)

target_include_directories(material_ui_legacy PRIVATE ${PROJECT_SOURCE_DIR}/editor_legacy/workbench/sinclude)

add_subdirectory(module)
