generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_EDITOR_MATERIAL_GRAPH_LIBRARY
    PUBLIC_MACRO_NAME       LUX_EDITOR_MATERIAL_GRAPH_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_EDITOR_MATERIAL_GRAPH_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/editor/material/visibility.h
)

add_component(
    COMPONENT_NAME editor_material_graph
    NAMESPACE      lux::engine::editor
    OUTPUT_NAME    lux_engine_editor_material_graph
    SOURCE_FILES src/MaterialGraphEditorAdapter.cpp
)
component_include_directories(
    editor_material_graph
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(editor_material_graph PRIVATE LUX_EDITOR_MATERIAL_GRAPH_LIBRARY)
target_link_libraries(
    editor_material_graph
    PUBLIC
        lux::engine::editor::node_graph_editor
        lux::engine::function::material_graph
)
component_add_transitive_commands(
    editor_material_graph
    "find_package(lux-engine-editor-node-graph REQUIRED COMPONENTS node_graph_editor)"
    "find_package(lux-engine-function REQUIRED COMPONENTS material_graph)"
)
lux_classify_target(
    TARGET  editor_material_graph
    LAYER   EDITOR
    PRODUCT EDITOR
    ROLE    INTEGRATION
)
install_components(
    PROJECT_NAME lux-engine-editor-material-graph
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::editor
    COMPONENTS   editor_material_graph
)
