cmake_minimum_required(VERSION 3.25)
project(lux_object_installed_consumer LANGUAGES CXX)

find_package(lux-engine-core REQUIRED COMPONENTS object)

add_executable(lux_object_installed_consumer main.cpp)
target_compile_features(lux_object_installed_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_object_installed_consumer
    PRIVATE lux::engine::core::object
)
