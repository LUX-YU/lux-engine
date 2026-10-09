cmake_minimum_required(VERSION 3.28)

project(FlowAnalysisConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-function REQUIRED COMPONENTS flowforge)

enable_testing()

add_executable(flow_control_flow "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test/control_flow.cpp")

target_link_libraries(flow_control_flow PRIVATE lux::engine::function::flowforge)

add_test(NAME installed.flow_control_without_toolchain COMMAND flow_control_flow)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/control_header.cpp" "#include <lux/engine/flowforge/FlowControlFlow.hpp>\n")

add_library(flow_control_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/control_header.cpp")

target_link_libraries(flow_control_header PRIVATE lux::engine::function::flowforge)

add_executable(flow_analysis "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test/analysis.cpp")

target_link_libraries(flow_analysis PRIVATE lux::engine::function::flowforge)

add_test(NAME installed.flow_analysis_without_toolchain COMMAND flow_analysis)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "#include <lux/engine/flowforge/FlowAnalysis.hpp>\n")

add_library(flow_analysis_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")

target_link_libraries(flow_analysis_header PRIVATE lux::engine::function::flowforge)


add_executable(source_codec "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test/source_codec.cpp")

target_link_libraries(source_codec PRIVATE lux::engine::function::flowforge)

add_test(
    NAME installed.flow_source_codec
    COMMAND source_codec "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test/fixtures/source-v1"
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/source_header.cpp" "#include <lux/engine/flowforge/graph/FlowSource.hpp>\n")

add_library(flow_source_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/source_header.cpp")

target_link_libraries(flow_source_header PRIVATE lux::engine::function::flowforge)

add_executable(
    connection_authority
    "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test/connection_authority.cpp"
)

target_link_libraries(connection_authority PRIVATE lux::engine::function::flowforge)

add_test(NAME installed.flow_connection_authority COMMAND connection_authority)

foreach(target flow_analysis flow_analysis_header flow_control_flow flow_control_header source_codec flow_source_header connection_authority)
    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endforeach()
