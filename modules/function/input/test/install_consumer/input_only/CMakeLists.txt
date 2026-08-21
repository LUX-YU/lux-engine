cmake_minimum_required(VERSION 3.25)
project(lux_input_only_consumer LANGUAGES CXX)

find_package(lux-engine-function REQUIRED COMPONENTS input)

add_executable(lux_input_only_consumer main.cpp)
target_compile_features(lux_input_only_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_input_only_consumer
    PRIVATE
        lux::engine::function::input
)
