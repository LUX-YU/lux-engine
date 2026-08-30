cmake_minimum_required(VERSION 3.22)
project(lux_resource_identity_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lux-engine-resource REQUIRED COMPONENTS identity)

add_executable(resource_identity_consumer main.cpp)
target_link_libraries(
    resource_identity_consumer
    PRIVATE lux::engine::resource::identity
)
