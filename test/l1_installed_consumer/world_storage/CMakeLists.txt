cmake_minimum_required(VERSION 3.22)
project(lux_l1_world_storage_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-world-storage
    CONFIG REQUIRED
    COMPONENTS world_storage
)

add_executable(lux_l1_world_storage_consumer main.cpp)
target_link_libraries(
    lux_l1_world_storage_consumer
    PRIVATE lux::engine::world::storage
)
