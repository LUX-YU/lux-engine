cmake_minimum_required(VERSION 3.22)
project(lux_l1_object_affinity_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lux-engine-core CONFIG REQUIRED COMPONENTS object)
find_package(lux-engine-ecs CONFIG REQUIRED COMPONENTS core system)

add_executable(lux_l1_object_affinity_consumer main.cpp)
target_link_libraries(
    lux_l1_object_affinity_consumer
    PRIVATE
        lux::engine::ecs::system
        lux::engine::core::object
)
