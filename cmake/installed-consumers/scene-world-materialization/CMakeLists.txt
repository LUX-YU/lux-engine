cmake_minimum_required(VERSION 3.22)
project(lux_scene_world_materialization_consumer LANGUAGES CXX)
find_package(lux-engine-scene-world-materialization REQUIRED COMPONENTS scene_world_materialization)
add_executable(lux_scene_world_materialization_consumer main.cpp)
target_compile_features(lux_scene_world_materialization_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_scene_world_materialization_consumer
    PRIVATE lux::engine::scene::scene_world_materialization
)
