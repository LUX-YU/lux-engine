cmake_minimum_required(VERSION 3.22)
project(lux_simulation_asset_consumer LANGUAGES CXX)

find_package(
    lux-engine-simulation-asset
    REQUIRED COMPONENTS simulation_asset
)

add_executable(lux_simulation_asset_consumer main.cpp)
target_compile_features(lux_simulation_asset_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_simulation_asset_consumer
    PRIVATE lux::engine::simulation::asset
)
