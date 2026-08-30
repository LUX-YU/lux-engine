cmake_minimum_required(VERSION 3.22)
project(lux_domain_world_identity_consumer LANGUAGES CXX)

find_package(lux-engine-domain-world-identity REQUIRED COMPONENTS domain_world_identity)

add_executable(lux_domain_world_identity_consumer main.cpp)
target_compile_features(lux_domain_world_identity_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_domain_world_identity_consumer
    PRIVATE lux::engine::domain::domain_world_identity
)
