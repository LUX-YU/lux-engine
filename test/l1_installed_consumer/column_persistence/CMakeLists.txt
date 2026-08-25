cmake_minimum_required(VERSION 3.22)
project(lux_l1_column_persistence_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-ecs
    CONFIG REQUIRED
    COMPONENTS persistence_contract persistence
)

foreach(target IN ITEMS
    lux::engine::ecs::persistence_contract
    lux::engine::ecs::persistence
    lux::engine::core::serialization
    lux::engine::core::type_info
)
    get_target_property(target_links ${target} INTERFACE_LINK_LIBRARIES)
    if("${target_links}" MATCHES "core::meta|reflection_runtime")
        message(FATAL_ERROR
            "column persistence acquired runtime reflection through ${target}: ${target_links}"
        )
    endif()
endforeach()

add_executable(lux_l1_column_persistence_consumer main.cpp)
target_link_libraries(
    lux_l1_column_persistence_consumer
    PRIVATE
        lux::engine::ecs::persistence_contract
        lux::engine::ecs::persistence
)
