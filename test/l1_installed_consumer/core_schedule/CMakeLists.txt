cmake_minimum_required(VERSION 3.22)
project(lux_l1_core_schedule_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-ecs
    CONFIG REQUIRED
    COMPONENTS core schedule
)

get_target_property(
    schedule_links
    lux::engine::ecs::schedule
    INTERFACE_LINK_LIBRARIES
)
if("${schedule_links}" MATCHES "object|reflection")
    message(FATAL_ERROR
        "core + schedule consumer acquired an Object/reflection closure: ${schedule_links}"
    )
endif()

add_executable(lux_l1_core_schedule_consumer main.cpp)
target_link_libraries(
    lux_l1_core_schedule_consumer
    PRIVATE lux::engine::ecs::schedule
)
