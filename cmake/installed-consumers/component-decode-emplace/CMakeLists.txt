cmake_minimum_required(VERSION 3.22)
project(lux_component_decode_consumer LANGUAGES CXX)

find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS transform)

add_executable(lux_component_decode_consumer main.cpp)
target_compile_features(lux_component_decode_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_component_decode_consumer
    PRIVATE lux::engine::simulation::ecs::transform
)
