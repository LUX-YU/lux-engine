cmake_minimum_required(VERSION 3.22)
project(lux_material_graph_consumer LANGUAGES CXX)

find_package(lux-engine-function REQUIRED COMPONENTS material_graph)

add_executable(lux_material_graph_consumer main.cpp)
target_compile_features(lux_material_graph_consumer PRIVATE cxx_std_20)
target_link_libraries(lux_material_graph_consumer PRIVATE lux::engine::function::material_graph)
