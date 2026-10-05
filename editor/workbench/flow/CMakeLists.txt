find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME flow_ui NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/FlowNodeControls.cpp src/FlowView.cpp)
component_include_directories(flow_ui BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flow_ui PUBLIC lux::engine::editor::flowforge_interaction lux::engine::editor::flowforge_compilation
    lux::engine::editor::view_api lux::engine::editor::editor_widgets PRIVATE lux::engine::editor::editor_composition imgui::core)
component_add_transitive_commands(flow_ui
    "find_package(lux-engine-editor-flowforge-interaction REQUIRED COMPONENTS flowforge_interaction)"
    "find_package(lux-engine-editor-flowforge-compilation REQUIRED COMPONENTS flowforge_compilation)"
    "find_package(lux-engine-editor-view-api REQUIRED COMPONENTS view_api)"
    "find_package(lux-engine-editor-widgets REQUIRED COMPONENTS editor_widgets)"
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET flow_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flow-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flow_ui)

target_include_directories(flow_ui PRIVATE ${PROJECT_SOURCE_DIR}/editor/workbench/sinclude)

add_subdirectory(module)
