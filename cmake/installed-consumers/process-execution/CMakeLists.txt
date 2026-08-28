cmake_minimum_required(VERSION 3.22)
project(lux_process_execution_consumer LANGUAGES CXX)

find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)

add_executable(lux_process_execution_consumer main.cpp)
target_compile_features(lux_process_execution_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_process_execution_consumer
    PRIVATE lux::engine::process::process_execution
)
