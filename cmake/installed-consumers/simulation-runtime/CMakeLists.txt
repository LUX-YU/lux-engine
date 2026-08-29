cmake_minimum_required(VERSION 3.22)
project(lux_simulation_runtime_consumer LANGUAGES CXX)

find_package(lux-engine-simulation-core REQUIRED COMPONENTS simulation_core)

add_executable(lux_simulation_runtime_consumer main.cpp)
target_compile_features(lux_simulation_runtime_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_simulation_runtime_consumer
    PRIVATE lux::engine::simulation::simulation_core
)
