cmake_minimum_required(VERSION 3.22)
project(lux_l1_core_system_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-ecs
    CONFIG REQUIRED
    COMPONENTS core system
)

get_target_property(
    system_links
    lux::engine::ecs::system
    INTERFACE_LINK_LIBRARIES
)
if("${system_links}" MATCHES "object|reflection")
    message(FATAL_ERROR
        "core + system consumer acquired an Object/reflection closure: ${system_links}"
    )
endif()

add_executable(lux_l1_core_system_consumer main.cpp)
target_link_libraries(
    lux_l1_core_system_consumer
    PRIVATE lux::engine::ecs::system
)
