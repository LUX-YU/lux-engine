cmake_minimum_required(VERSION 3.22)
project(lux_ecs_snapshot_consumer LANGUAGES CXX)

find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core schema snapshot)

add_executable(lux_ecs_snapshot_consumer main.cpp)
target_compile_features(lux_ecs_snapshot_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_ecs_snapshot_consumer
    PRIVATE lux::engine::simulation::ecs::snapshot
)
