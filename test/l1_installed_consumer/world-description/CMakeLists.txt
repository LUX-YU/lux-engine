cmake_minimum_required(VERSION 3.22)
project(lux_l1_world_description_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lux-engine-world-description CONFIG REQUIRED COMPONENTS world_description)

add_executable(lux_l1_world_description_consumer main.cpp)
target_link_libraries(
    lux_l1_world_description_consumer
    PRIVATE lux::engine::world::description
)
