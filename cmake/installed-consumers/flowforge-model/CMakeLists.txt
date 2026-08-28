cmake_minimum_required(VERSION 3.22)
project(lux_flowforge_model_consumer LANGUAGES CXX)

find_package(lux-engine-function REQUIRED COMPONENTS flowforge)

add_executable(lux_flowforge_model_consumer main.cpp)
target_compile_features(lux_flowforge_model_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_flowforge_model_consumer
    PRIVATE lux::engine::function::flowforge
)
