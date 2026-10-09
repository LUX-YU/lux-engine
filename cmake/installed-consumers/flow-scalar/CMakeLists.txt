cmake_minimum_required(VERSION 3.28)

project(FlowScalarConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)

find_package(lux-engine-function REQUIRED COMPONENTS script_native)

find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)

enable_testing()

add_executable(scalar_lowering "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/scalar_lowering.cpp")

target_link_libraries(scalar_lowering PRIVATE lux::engine::flowforge::flowforge_compiler)

if(MSVC)
    target_compile_options(scalar_lowering PRIVATE /permissive- /GR- /utf-8)
else()
    target_compile_options(scalar_lowering PRIVATE -fno-rtti)
endif()

add_test(NAME installed.flow_scalar_lowering COMMAND scalar_lowering)

add_executable(control_lowering "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/control_lowering.cpp")

target_link_libraries(control_lowering PRIVATE lux::engine::flowforge::flowforge_compiler)

add_test(NAME installed.flow_control_compiler_compatibility COMMAND control_lowering)

add_executable(nested_control "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/nested_control.cpp")

target_link_libraries(
    nested_control
    PRIVATE lux::engine::flowforge::flowforge_compiler lux::engine::function::script_native
)

add_test(NAME installed.flow_nested_control COMMAND nested_control)

add_library(
    registered_value_plugin MODULE
    "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/registered_value_plugin.cpp"
)

target_link_libraries(registered_value_plugin PRIVATE lux::engine::function::flowforge)

add_executable(
    registered_value
    "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/registered_value.cpp"
)

target_link_libraries(
    registered_value
    PRIVATE lux::engine::flowforge::flowforge_compiler lux::engine::function::script_native
        lux::engine::platform::dynamic_library
)

add_dependencies(registered_value registered_value_plugin)

add_test(NAME installed.flow_registered_value COMMAND registered_value)

add_test(
    NAME installed.flow_registered_value_plugin
    COMMAND registered_value "$<TARGET_FILE:registered_value_plugin>"
)

add_test(
    NAME installed.flow_registered_value_foreign_definition
    COMMAND registered_value "$<TARGET_FILE:registered_value_plugin>" definition
)

add_executable(
    node_catalog
    "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test/node_catalog.cpp"
)

target_link_libraries(node_catalog PRIVATE lux::engine::function::flowforge)

add_test(NAME installed.flow_node_catalog COMMAND node_catalog)

foreach(target registered_value registered_value_plugin node_catalog)
    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endforeach()

foreach(header FlowNodeCatalog FlowValueCompiler ScalarNodes)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" "#include <lux/engine/flowforge/${header}.hpp>\n")

    add_library(header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

    target_link_libraries(header_${header} PRIVATE lux::engine::function::flowforge)
endforeach()
