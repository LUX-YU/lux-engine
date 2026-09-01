cmake_minimum_required(VERSION 3.22)
project(lux_l1_system_foundation_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(
    lux-engine-domain-system-identity
    CONFIG REQUIRED
    COMPONENTS domain_system_identity
)
find_package(
    lux-engine-domain-system-description
    CONFIG REQUIRED
    COMPONENTS domain_system_description
)

add_executable(lux_l1_system_foundation_consumer main.cpp)
target_link_libraries(
    lux_l1_system_foundation_consumer
    PRIVATE lux::engine::system::domain_system_description
)
