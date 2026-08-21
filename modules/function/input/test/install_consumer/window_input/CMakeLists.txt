cmake_minimum_required(VERSION 3.25)
project(lux_window_input_consumer LANGUAGES CXX)

find_package(lux-engine-platform REQUIRED COMPONENTS window)
find_package(lux-engine-function REQUIRED COMPONENTS input)

add_executable(lux_window_input_consumer main.cpp)
target_compile_features(lux_window_input_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_window_input_consumer
    PRIVATE
        lux::engine::platform::window
        lux::engine::function::input
)
