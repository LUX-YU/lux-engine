cmake_minimum_required(VERSION 3.22)
project(lux_transform_registration_consumer LANGUAGES CXX)

find_package(lux-engine-simulation REQUIRED COMPONENTS transform_system)

add_executable(lux_transform_registration_consumer main.cpp)
target_compile_features(lux_transform_registration_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_transform_registration_consumer
    PRIVATE lux::engine::simulation::transform_system
)
