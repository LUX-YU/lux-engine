cmake_minimum_required(VERSION 3.22)
project(lux_l0_core_task_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-core
    CONFIG REQUIRED
    COMPONENTS task
)

add_executable(lux_l0_core_task_consumer main.cpp)
target_link_libraries(
    lux_l0_core_task_consumer
    PRIVATE lux::engine::core::task
)
