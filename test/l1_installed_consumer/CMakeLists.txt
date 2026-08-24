cmake_minimum_required(VERSION 3.22)
project(lux_l1_installed_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-resource
    CONFIG REQUIRED
    COMPONENTS asset
)
find_package(
    lux-engine-ecs
    CONFIG REQUIRED
    COMPONENTS core schedule schema snapshot persistence hierarchy transform
)

add_executable(lux_l1_installed_consumer main.cpp)
target_link_libraries(
    lux_l1_installed_consumer
    PRIVATE
        lux::engine::resource::asset
        lux::engine::ecs::transform
        lux::engine::ecs::snapshot
        lux::engine::ecs::persistence
)
