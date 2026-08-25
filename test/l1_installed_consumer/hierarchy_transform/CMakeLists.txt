cmake_minimum_required(VERSION 3.22)
project(lux_l1_hierarchy_transform_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-ecs
    CONFIG REQUIRED
    COMPONENTS core system schema snapshot world_section hierarchy transform
)

add_executable(lux_l1_hierarchy_transform_consumer main.cpp)
target_link_libraries(
    lux_l1_hierarchy_transform_consumer
    PRIVATE
        lux::engine::ecs::hierarchy
        lux::engine::ecs::transform
)
