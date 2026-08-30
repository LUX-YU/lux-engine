cmake_minimum_required(VERSION 3.22)
project(lux_spatial2d_index_consumer LANGUAGES CXX)

find_package(lux-engine-domain-spatial2d REQUIRED COMPONENTS spatial2d_index)

add_executable(lux_spatial2d_index_consumer main.cpp)
target_compile_features(lux_spatial2d_index_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_spatial2d_index_consumer
    PRIVATE lux::engine::spatial::spatial2d_index
)
