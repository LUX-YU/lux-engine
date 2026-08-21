cmake_minimum_required(VERSION 3.25)
project(resource_asset_install_consumer LANGUAGES CXX)

find_package(lux-engine-resource REQUIRED COMPONENTS asset)

add_executable(resource_asset_install_consumer main.cpp)
target_compile_features(resource_asset_install_consumer PRIVATE cxx_std_20)
target_link_libraries(
    resource_asset_install_consumer
    PRIVATE lux::engine::resource::asset
)
