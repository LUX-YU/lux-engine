cmake_minimum_required(VERSION 3.22)
project(lux_l1_ecs_system_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lux-engine-simulation-system CONFIG REQUIRED COMPONENTS system)

add_executable(lux_l1_ecs_system_consumer main.cpp)
target_link_libraries(
    lux_l1_ecs_system_consumer
    PRIVATE lux::engine::simulation::system
)
