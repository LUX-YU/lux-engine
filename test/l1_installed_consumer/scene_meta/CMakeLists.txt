cmake_minimum_required(VERSION 3.22)
project(lux_l3_scene_meta_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lux-engine-scene-meta CONFIG REQUIRED COMPONENTS scene_meta)

add_executable(lux_l3_scene_meta_consumer main.cpp)
target_link_libraries(lux_l3_scene_meta_consumer PRIVATE lux::engine::scene::scene_meta)
