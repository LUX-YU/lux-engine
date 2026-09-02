cmake_minimum_required(VERSION 3.22)
project(lux_asset_vfs_view_consumer LANGUAGES CXX)

find_package(lux-engine-resource REQUIRED COMPONENTS asset)

add_executable(lux_asset_vfs_view_consumer main.cpp)
target_compile_features(lux_asset_vfs_view_consumer PRIVATE cxx_std_20)
target_link_libraries(lux_asset_vfs_view_consumer PRIVATE lux::engine::resource::asset)
