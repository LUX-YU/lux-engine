cmake_minimum_required(VERSION 3.25)
project(lux_editor_inspector_consumer LANGUAGES CXX)

find_package(
    lux-engine-editor-inspector
    REQUIRED COMPONENTS editor_inspector
)

add_executable(lux_editor_inspector_consumer main.cpp)
target_compile_features(lux_editor_inspector_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_editor_inspector_consumer
    PRIVATE lux::engine::editor::editor_inspector
)
