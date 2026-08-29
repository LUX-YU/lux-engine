cmake_minimum_required(VERSION 3.22)
project(lux_world_storage_consumer LANGUAGES CXX)

find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)

add_executable(lux_world_storage_consumer main.cpp)
target_compile_features(lux_world_storage_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_world_storage_consumer
    PRIVATE lux::engine::world::storage
)
