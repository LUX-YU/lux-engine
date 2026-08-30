cmake_minimum_required(VERSION 3.22)
project(lux_typed_resource_assets_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lux-engine-resource REQUIRED COMPONENTS asset)

add_executable(typed_resource_assets_consumer main.cpp)
target_link_libraries(
    typed_resource_assets_consumer
    PRIVATE lux::engine::resource::asset
)
