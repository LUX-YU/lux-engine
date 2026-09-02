cmake_minimum_required(VERSION 3.22)
project(lux_editor_context_consumer LANGUAGES CXX)

find_package(
    lux-engine-editor-context
    REQUIRED COMPONENTS editor_context
)

add_executable(lux_editor_context_consumer main.cpp)
target_compile_features(lux_editor_context_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_editor_context_consumer
    PRIVATE lux::engine::editor::editor_context
)
