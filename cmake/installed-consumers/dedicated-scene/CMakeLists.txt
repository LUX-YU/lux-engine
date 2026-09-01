cmake_minimum_required(VERSION 3.22)
project(lux_dedicated_scene_consumer LANGUAGES CXX)

find_package(lux-engine-scene REQUIRED COMPONENTS scene_core)
find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)
find_package(lux-engine-world-description REQUIRED COMPONENTS world_description)

add_executable(lux_dedicated_scene_consumer main.cpp)
target_compile_features(lux_dedicated_scene_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_dedicated_scene_consumer
    PRIVATE
        lux::engine::scene::scene_core
        lux::engine::scene::scene_description
        lux::engine::simulation::description
        lux::engine::world::description
)
