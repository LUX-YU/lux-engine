cmake_minimum_required(VERSION 3.22)
project(lux_partition_identity_consumer LANGUAGES CXX)

find_package(lux-engine-partition-identity REQUIRED COMPONENTS partition_identity)

add_executable(lux_partition_identity_consumer main.cpp)
target_compile_features(lux_partition_identity_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_partition_identity_consumer
    PRIVATE lux::engine::partition::identity
)
