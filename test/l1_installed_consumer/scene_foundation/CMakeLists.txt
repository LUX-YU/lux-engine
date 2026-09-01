cmake_minimum_required(VERSION 3.22)
project(lux_l3_scene_foundation_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lux-engine-scene-description CONFIG REQUIRED COMPONENTS scene_description)
find_package(lux-engine-scene-system CONFIG REQUIRED COMPONENTS scene_system)

add_executable(lux_l3_scene_foundation_consumer main.cpp)
target_link_libraries(
    lux_l3_scene_foundation_consumer
    PRIVATE lux::engine::scene::scene_system
)
