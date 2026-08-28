cmake_minimum_required(VERSION 3.22)
project(lux_node_graph_editor_consumer LANGUAGES CXX)

find_package(
    lux-engine-editor-node-graph
    REQUIRED COMPONENTS node_graph_editor
)

add_executable(lux_node_graph_editor_consumer main.cpp)
target_compile_features(lux_node_graph_editor_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_node_graph_editor_consumer
    PRIVATE lux::engine::editor::node_graph_editor
)
