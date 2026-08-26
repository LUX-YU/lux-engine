cmake_minimum_required(VERSION 3.22)
project(lux_ecs_core_system_consumer LANGUAGES CXX)

find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)
find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)
find_package(lux-engine-core REQUIRED COMPONENTS task)

add_executable(lux_ecs_core_system_consumer main.cpp)
target_compile_features(lux_ecs_core_system_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_ecs_core_system_consumer
    PRIVATE
        lux::engine::simulation::ecs::core
        lux::engine::simulation::system
        lux::engine::core::task
)
