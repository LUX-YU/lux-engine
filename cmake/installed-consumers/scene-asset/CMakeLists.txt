cmake_minimum_required(VERSION 3.22)
project(lux_scene_asset_consumer LANGUAGES CXX)

find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)

add_executable(lux_scene_asset_consumer main.cpp)
target_compile_features(lux_scene_asset_consumer PRIVATE cxx_std_20)
target_link_libraries(lux_scene_asset_consumer PRIVATE lux::engine::scene::scene_asset)
