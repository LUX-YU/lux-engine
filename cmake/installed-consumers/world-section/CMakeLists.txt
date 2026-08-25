cmake_minimum_required(VERSION 3.22)
project(lux_ecs_world_section_consumer LANGUAGES CXX)

find_package(lux-engine-ecs REQUIRED COMPONENTS core schema world_section)

add_executable(lux_ecs_world_section_consumer main.cpp)
target_compile_features(lux_ecs_world_section_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_ecs_world_section_consumer
    PRIVATE lux::engine::ecs::world_section
)
