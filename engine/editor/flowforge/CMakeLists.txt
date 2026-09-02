generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_EDITOR_FLOW_GRAPH_LIBRARY
    PUBLIC_MACRO_NAME       LUX_EDITOR_FLOW_GRAPH_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_EDITOR_FLOW_GRAPH_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/editor/flowforge/visibility.h
)

add_component(
    COMPONENT_NAME editor_flow_graph
    NAMESPACE      lux::engine::editor
    OUTPUT_NAME    lux_engine_editor_flow_graph
    SOURCE_FILES src/FlowGraphEditorAdapter.cpp
)
component_include_directories(
    editor_flow_graph
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(editor_flow_graph PRIVATE LUX_EDITOR_FLOW_GRAPH_LIBRARY)
target_link_libraries(
    editor_flow_graph
    PUBLIC
        lux::engine::editor::node_graph_editor
        lux::engine::function::flowforge
)
component_add_transitive_commands(
    editor_flow_graph
    "find_package(lux-engine-editor-node-graph REQUIRED COMPONENTS node_graph_editor)"
    "find_package(lux-engine-function REQUIRED COMPONENTS flowforge)"
)
lux_classify_target(
    TARGET  editor_flow_graph
    LAYER   EDITOR
    PRODUCT EDITOR
    ROLE    INTEGRATION
)

add_executable(graph_domain_adapters_test test/graph_domain_adapters_test.cpp)
target_link_libraries(
    graph_domain_adapters_test
    PRIVATE
        editor_flow_graph
        lux::engine::editor::editor_material_graph
)
if(MSVC)
    target_compile_options(graph_domain_adapters_test PRIVATE /UNDEBUG)
else()
    target_compile_options(graph_domain_adapters_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  graph_domain_adapters_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME graph_domain_adapters COMMAND graph_domain_adapters_test)

install_components(
    PROJECT_NAME lux-engine-editor-flow-graph
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::editor
    COMPONENTS   editor_flow_graph
)
