cmake_minimum_required(VERSION 3.22)
project(lux_core_task_consumer LANGUAGES CXX)

find_package(lux-engine-core REQUIRED COMPONENTS task)

add_executable(lux_core_task_consumer main.cpp)
target_compile_features(lux_core_task_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_core_task_consumer
    PRIVATE lux::engine::core::task
)
