cmake_minimum_required(VERSION 3.22)
project(lux_l1_object_affinity_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lux-engine-core CONFIG REQUIRED COMPONENTS object task)
find_package(lux-engine-simulation-ecs CONFIG REQUIRED COMPONENTS core)
find_package(lux-engine-simulation-system CONFIG REQUIRED COMPONENTS system)

add_executable(lux_l1_object_affinity_consumer main.cpp)
target_link_libraries(
    lux_l1_object_affinity_consumer
    PRIVATE
        lux::engine::simulation::system
        lux::engine::core::object
        lux::engine::core::task
)
