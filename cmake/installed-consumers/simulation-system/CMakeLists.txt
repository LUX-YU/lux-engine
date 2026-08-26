cmake_minimum_required(VERSION 3.22)
project(lux_simulation_system_consumer LANGUAGES CXX)

find_package(
    lux-engine-simulation-system
    REQUIRED COMPONENTS system
)

add_executable(lux_simulation_system_consumer main.cpp)
target_compile_features(lux_simulation_system_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_simulation_system_consumer
    PRIVATE lux::engine::simulation::system
)
