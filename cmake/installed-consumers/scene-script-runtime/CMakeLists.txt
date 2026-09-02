cmake_minimum_required(VERSION 3.22)
project(lux_scene_script_runtime_consumer LANGUAGES CXX)

find_package(lux-engine-scene-script-runtime REQUIRED COMPONENTS scene_script_runtime)

add_executable(lux_scene_script_runtime_consumer main.cpp)
target_compile_features(lux_scene_script_runtime_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_scene_script_runtime_consumer
    PRIVATE lux::engine::scene::scene_script_runtime
)
