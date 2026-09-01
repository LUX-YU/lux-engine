cmake_minimum_required(VERSION 3.22)
project(lux_material_cooker_consumer LANGUAGES CXX)

find_package(
    lux-engine-toolchain-material
    REQUIRED COMPONENTS toolchain_material_cooker
)

add_executable(lux_material_cooker_consumer main.cpp)
target_compile_features(lux_material_cooker_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_material_cooker_consumer
    PRIVATE lux::engine::toolchain::toolchain_material_cooker
)
