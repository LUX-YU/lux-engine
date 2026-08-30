cmake_minimum_required(VERSION 3.22)
project(lux_resource_descriptions_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lux-engine-resource REQUIRED COMPONENTS description)

add_executable(resource_descriptions_consumer main.cpp)
target_link_libraries(
    resource_descriptions_consumer
    PRIVATE lux::engine::resource::description
)
