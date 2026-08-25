cmake_minimum_required(VERSION 3.22)
project(lux_l1_snapshot_world_section_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-ecs
    CONFIG REQUIRED
    COMPONENTS core schema snapshot world_section
)

foreach(component IN ITEMS snapshot world_section)
    get_target_property(
        component_links
        lux::engine::ecs::${component}
        INTERFACE_LINK_LIBRARIES
    )
    if("${component_links}" MATCHES "ecs::system|core::object|reflection")
        message(FATAL_ERROR
            "${component} acquired execution/Object/reflection closure: ${component_links}"
        )
    endif()
endforeach()

add_executable(lux_l1_snapshot_world_section_consumer main.cpp)
target_link_libraries(
    lux_l1_snapshot_world_section_consumer
    PRIVATE
        lux::engine::ecs::snapshot
        lux::engine::ecs::world_section
)
