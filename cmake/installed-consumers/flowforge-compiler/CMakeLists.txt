cmake_minimum_required(VERSION 3.22)
project(lux_flowforge_compiler_consumer LANGUAGES CXX)

find_package(
    lux-engine-flowforge-compiler
    REQUIRED COMPONENTS flowforge_script_compiler
)

add_executable(lux_flowforge_compiler_consumer main.cpp)
target_compile_features(lux_flowforge_compiler_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_flowforge_compiler_consumer
    PRIVATE lux::engine::flowforge::flowforge_script_compiler
)
