cmake_minimum_required(VERSION 3.25)
project(scene_asset_install_consumer LANGUAGES CXX)

find_package(lux-engine-resource REQUIRED COMPONENTS asset)
find_package(lux-engine-scene REQUIRED COMPONENTS scene)

add_executable(scene_asset_install_consumer main.cpp)
target_compile_features(scene_asset_install_consumer PRIVATE cxx_std_20)
target_link_libraries(
    scene_asset_install_consumer
    PRIVATE
        lux::engine::resource::asset
        lux::engine::scene::scene
)
