cmake_minimum_required(VERSION 3.22)
project(lux_process_world_consumer LANGUAGES CXX)

find_package(lux-engine-process-world REQUIRED COMPONENTS process_world)

add_executable(lux_process_world_consumer main.cpp)
target_compile_features(lux_process_world_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_process_world_consumer
    PRIVATE lux::engine::process::process_world
)
