cmake_minimum_required(VERSION 3.28)

project(FlowNativeConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-function REQUIRED COMPONENTS flowforge)
find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)
find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)

enable_testing()

set(flow_test "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test")

add_executable(native_definition "${flow_test}/native_definition.cpp")
target_link_libraries(native_definition PRIVATE lux::engine::function::flowforge)
add_test(NAME installed.flow_native_definition COMMAND native_definition)

add_library(native_definition_plugin MODULE "${flow_test}/native_definition_plugin.cpp")
target_link_libraries(native_definition_plugin PRIVATE lux::engine::function::flowforge)

add_executable(native_definition_plugin_test "${flow_test}/native_definition_plugin_test.cpp")
target_link_libraries(
    native_definition_plugin_test
    PRIVATE lux::engine::function::flowforge lux::engine::platform::dynamic_library
)
add_dependencies(native_definition_plugin_test native_definition_plugin)
add_test(NAME installed.flow_native_dll COMMAND native_definition_plugin_test "$<TARGET_FILE:native_definition_plugin>")

add_executable(native_compiler "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/native_definition.cpp")
target_link_libraries(native_compiler PRIVATE lux::engine::flowforge::flowforge_compiler)
add_test(NAME installed.flow_native_compiler COMMAND native_compiler)

add_executable(structure_access "${flow_test}/structure_access.cpp")

target_link_libraries(structure_access PRIVATE lux::engine::function::flowforge)

add_test(NAME installed.flow_structure_access COMMAND structure_access)

add_executable(flow_analysis "${flow_test}/analysis.cpp")

target_link_libraries(flow_analysis PRIVATE lux::engine::function::flowforge)

add_test(NAME installed.flow_analysis COMMAND flow_analysis)

add_executable(flow_analysis_compiler "${flow_test}/analysis.cpp")

target_link_libraries(flow_analysis_compiler PRIVATE lux::engine::flowforge::flowforge_compiler)

target_compile_definitions(flow_analysis_compiler PRIVATE FLOW_ANALYSIS_COMPILER=1)

add_test(NAME installed.flow_analysis_compiler COMMAND flow_analysis_compiler)

add_executable(async_lowering "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/async_lowering.cpp")

target_link_libraries(async_lowering PRIVATE lux::engine::flowforge::flowforge_compiler)

add_test(NAME installed.flow_async_lowering COMMAND async_lowering)

foreach(target native_definition native_definition_plugin native_definition_plugin_test native_compiler structure_access
    flow_analysis flow_analysis_compiler async_lowering
)
    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endforeach()

foreach(header
    FlowAnalysis NativeCallDefinition FunctionNodes ControlNodes ObjectNodes FlowExecutionCompiler
    graph/NodeRegistry graph/FlowGraph graph/FlowNode
)
    string(REPLACE "/" "_" stem "${header}")

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${stem}.cpp" "#include <lux/engine/flowforge/${header}.hpp>\n")

    add_library(header_${stem} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${stem}.cpp")

    target_link_libraries(header_${stem} PRIVATE lux::engine::function::flowforge)
endforeach()
