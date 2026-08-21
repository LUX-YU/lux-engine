cmake_minimum_required(VERSION 3.24)
project(lux_script_core_installed_consumer LANGUAGES CXX)

find_package(lux-engine-function REQUIRED COMPONENTS script_core)

add_executable(script_core_installed_consumer main.cpp)
target_compile_features(script_core_installed_consumer PRIVATE cxx_std_20)
target_link_libraries(
    script_core_installed_consumer
    PRIVATE
        lux::engine::function::script_core
)
