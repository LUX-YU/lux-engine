cmake_minimum_required(VERSION 3.22)
project(lux_editor_graph_adapters_consumer LANGUAGES CXX)

find_package(lux-engine-editor-material-graph REQUIRED COMPONENTS editor_material_graph)
find_package(lux-engine-editor-flow-graph REQUIRED COMPONENTS editor_flow_graph)

add_executable(lux_editor_graph_adapters_consumer main.cpp)
target_compile_features(lux_editor_graph_adapters_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_editor_graph_adapters_consumer
    PRIVATE
        lux::engine::editor::editor_material_graph
        lux::engine::editor::editor_flow_graph
)
