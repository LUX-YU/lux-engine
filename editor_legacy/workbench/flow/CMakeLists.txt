find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME flow_ui_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/FlowNodeControls.cpp src/FlowView.cpp)
component_include_directories(flow_ui_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flow_ui_legacy PUBLIC lux::engine::editor::flowforge_interaction_legacy lux::engine::editor::flowforge_compilation_legacy
    lux::engine::editor::editor_widgets_legacy lux::engine::editor::editor_composition_legacy PRIVATE imgui::core)
component_add_transitive_commands(flow_ui_legacy
    "find_package(lux-engine-editor-flowforge-interaction REQUIRED COMPONENTS flowforge_interaction_legacy)"
    "find_package(lux-engine-editor-flowforge-compilation REQUIRED COMPONENTS flowforge_compilation_legacy)"
    "find_package(lux-engine-editor-widgets REQUIRED COMPONENTS editor_widgets_legacy)"
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition_legacy)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET flow_ui_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flow-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flow_ui_legacy)

target_include_directories(flow_ui_legacy PRIVATE ${PROJECT_SOURCE_DIR}/editor_legacy/workbench/sinclude)

add_subdirectory(module)
