find_package(node_editor REQUIRED COMPONENTS node_editor)
add_component(COMPONENT_NAME editor_widgets NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/TreeRows.cpp src/NodeCanvas.cpp src/GraphCanvas.cpp)
component_include_directories(editor_widgets BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_widgets PUBLIC lux::engine::function::ui imgui::node_editor)
component_add_transitive_commands(editor_widgets "find_package(lux-engine-function REQUIRED COMPONENTS ui)")
component_add_transitive_commands(editor_widgets "find_package(node_editor REQUIRED COMPONENTS node_editor)")
lux_classify_target(TARGET editor_widgets LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-widgets VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_widgets)
