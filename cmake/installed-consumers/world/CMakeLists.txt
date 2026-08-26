cmake_minimum_required(VERSION 3.22)
project(lux_world_consumer LANGUAGES CXX)

find_package(lux-engine-world REQUIRED COMPONENTS world)

add_executable(lux_world_consumer main.cpp)
target_compile_features(lux_world_consumer PRIVATE cxx_std_20)
target_link_libraries(lux_world_consumer PRIVATE lux::engine::world)
