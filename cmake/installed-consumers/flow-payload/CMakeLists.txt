cmake_minimum_required(VERSION 3.28)

project(FlowPayloadConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-function REQUIRED COMPONENTS flowforge)
find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)

enable_testing()

set(flow_test_source "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test")

add_executable(flow_payload "${flow_test_source}/node_payload.cpp")

target_link_libraries(flow_payload PRIVATE lux::engine::function::flowforge)

add_library(flow_payload_plugin MODULE "${flow_test_source}/node_payload_plugin.cpp")

target_link_libraries(flow_payload_plugin PRIVATE lux::engine::function::flowforge)

add_executable(flow_payload_plugin_test "${flow_test_source}/node_payload_plugin_test.cpp")

target_link_libraries(
    flow_payload_plugin_test
    PRIVATE lux::engine::function::flowforge lux::engine::platform::dynamic_library
)

add_dependencies(flow_payload_plugin_test flow_payload_plugin)

add_test(NAME installed.flow_payload COMMAND flow_payload)

add_test(NAME installed.flow_payload_plugin COMMAND flow_payload_plugin_test $<TARGET_FILE:flow_payload_plugin>)

set(flow_targets flow_payload flow_payload_plugin flow_payload_plugin_test)

foreach(header FlowNodePayload FlowForgeFailure)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" "#include <lux/engine/flowforge/${header}.hpp>\n")

    add_library(${header}_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

    target_link_libraries(${header}_header PRIVATE lux::engine::function::flowforge)

    list(APPEND flow_targets ${header}_header)
endforeach()

option(FLOW_PAYLOAD_COMPILER "Also exercise the actual installed Flow compiler" OFF)

if(FLOW_PAYLOAD_COMPILER)
    find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)

    add_executable(flow_compiler "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/ability_contract.cpp")

    target_link_libraries(flow_compiler PRIVATE lux::engine::flowforge::flowforge_compiler)

    add_test(NAME installed.flow_compiler COMMAND flow_compiler)

    list(APPEND flow_targets flow_compiler)

    if(MSVC)
        target_compile_options(flow_compiler PRIVATE /UNDEBUG)
    else()
        target_compile_options(flow_compiler PRIVATE -UNDEBUG)
    endif()
endif()

foreach(target IN LISTS flow_targets)
    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endforeach()
