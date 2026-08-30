cmake_minimum_required(VERSION 3.22)
project(lux_typed_mature_assets_consumer LANGUAGES CXX)

find_package(lux-engine-resource REQUIRED COMPONENTS asset description)
find_package(lux-engine-function REQUIRED COMPONENTS script_artifact)

add_executable(lux_typed_mature_assets_consumer main.cpp)
target_compile_features(lux_typed_mature_assets_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_typed_mature_assets_consumer
    PRIVATE
        lux::engine::resource::asset
        lux::engine::function::script_artifact
)
