cmake_minimum_required(VERSION 3.22)
project(lux_typed_asset_consumer LANGUAGES CXX)

find_package(lux-engine-resource REQUIRED COMPONENTS asset)

add_executable(lux_typed_asset_consumer main.cpp)
target_compile_features(lux_typed_asset_consumer PRIVATE cxx_std_20)
target_link_libraries(lux_typed_asset_consumer PRIVATE lux::engine::resource::asset)
