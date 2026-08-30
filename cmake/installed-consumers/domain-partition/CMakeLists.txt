cmake_minimum_required(VERSION 3.22)
project(lux_domain_partition_consumer LANGUAGES CXX)

find_package(lux-engine-domain-partition REQUIRED COMPONENTS partition_core)

add_executable(lux_domain_partition_consumer main.cpp)
target_compile_features(lux_domain_partition_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_domain_partition_consumer
    PRIVATE lux::engine::partition::partition_core
)
