## Domain-independent node graph editing UI.

find_package(imgui REQUIRED COMPONENTS core)
find_package(node_editor REQUIRED COMPONENTS node_editor)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_NODE_GRAPH_EDITOR_LIBRARY
    PUBLIC_MACRO_NAME       LUX_NODE_GRAPH_EDITOR_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_NODE_GRAPH_EDITOR_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/editor/node_graph/visibility.h
)

set(LUX_NODE_GRAPH_EDITOR_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphCommandStack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphEditor.cpp
)

add_component(
    COMPONENT_NAME node_graph_editor
    STATIC
    NAMESPACE      lux::engine::editor
    OUTPUT_NAME    lux_engine_editor_node_graph
    SOURCE_FILES   ${LUX_NODE_GRAPH_EDITOR_SRCS}
)

lux_classify_target(
    TARGET node_graph_editor
    LAYER EDITOR
    PRODUCT EDITOR
    ROLE FOUNDATION
)

component_include_directories(
    node_graph_editor
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME
        include
)

target_compile_definitions(
    node_graph_editor
    PRIVATE
    LUX_NODE_GRAPH_EDITOR_LIBRARY
    PUBLIC
    LUX_NODE_GRAPH_EDITOR_DLL_DISABLE
)

target_link_libraries(
    node_graph_editor
    PUBLIC
    imgui::node_editor
)

component_add_transitive_commands(
    node_graph_editor
    "find_package(imgui REQUIRED COMPONENTS core)"
    "find_package(node_editor REQUIRED COMPONENTS node_editor)"
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

install_components(
    PROJECT_NAME lux-engine-editor-node-graph
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::editor
    COMPONENTS   node_graph_editor
)
