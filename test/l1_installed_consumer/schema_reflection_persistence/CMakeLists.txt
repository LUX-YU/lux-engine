cmake_minimum_required(VERSION 3.22)
project(lux_l1_schema_reflection_persistence_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-ecs
    CONFIG REQUIRED
    COMPONENTS schema_reflection persistence
)

add_executable(lux_l1_schema_reflection_persistence_consumer main.cpp)
target_link_libraries(
    lux_l1_schema_reflection_persistence_consumer
    PRIVATE
        lux::engine::ecs::schema_reflection
        lux::engine::ecs::persistence
)
