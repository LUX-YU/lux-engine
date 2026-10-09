cmake_minimum_required(VERSION 3.28)

project(FlowAnalysisConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-function REQUIRED COMPONENTS flowforge)

enable_testing()

add_executable(flow_analysis "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test/analysis.cpp")

target_link_libraries(flow_analysis PRIVATE lux::engine::function::flowforge)

add_test(NAME installed.flow_analysis_without_toolchain COMMAND flow_analysis)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "#include <lux/engine/flowforge/FlowAnalysis.hpp>\n")

add_library(flow_analysis_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")

target_link_libraries(flow_analysis_header PRIVATE lux::engine::function::flowforge)

foreach(target flow_analysis flow_analysis_header)
    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endforeach()
