cmake_minimum_required(VERSION 3.22)
project(lux_process_asset_loading_consumer LANGUAGES CXX)

find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)

add_executable(lux_process_asset_loading_consumer main.cpp)
target_compile_features(lux_process_asset_loading_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_process_asset_loading_consumer
    PRIVATE lux::engine::process::asset_loading
)
