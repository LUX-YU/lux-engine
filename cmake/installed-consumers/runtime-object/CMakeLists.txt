cmake_minimum_required(VERSION 3.28)

project(RuntimeObjectConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lux-engine-core REQUIRED COMPONENTS meta)

find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)

find_package(lux-engine-function REQUIRED COMPONENTS flowforge)

enable_testing()

set(meta_tests "${CMAKE_CURRENT_LIST_DIR}/../../../modules/core/meta/test")

add_executable(runtime_object "${meta_tests}/RuntimeObjectTest.cpp")

target_link_libraries(runtime_object PRIVATE lux::engine::core::meta)

add_test(NAME installed.runtime_object COMMAND runtime_object)

add_library(runtime_object_plugin SHARED "${meta_tests}/RuntimeObjectPlugin.cpp")

target_link_libraries(runtime_object_plugin PRIVATE lux::engine::core::meta)

add_executable(runtime_object_plugin_test "${meta_tests}/RuntimeObjectPluginTest.cpp")

target_link_libraries(runtime_object_plugin_test PRIVATE lux::engine::core::meta lux::engine::platform::dynamic_library)

add_test(
    NAME installed.runtime_object_plugin
    COMMAND runtime_object_plugin_test "$<TARGET_FILE:runtime_object_plugin>"
)

add_executable(flow_default "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test/constant_default.cpp")

target_link_libraries(flow_default PRIVATE lux::engine::function::flowforge)

add_test(NAME installed.flow_default COMMAND flow_default)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "#include <lux/engine/meta/RuntimeObject.hpp>\n")

add_library(runtime_object_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")

target_link_libraries(runtime_object_header PRIVATE lux::engine::core::meta)

if(MSVC)
    foreach(target runtime_object runtime_object_plugin runtime_object_plugin_test flow_default runtime_object_header)
        target_compile_options(${target} PRIVATE /permissive- /GR- /we4541 /utf-8)
    endforeach()
else()
    foreach(target runtime_object runtime_object_plugin runtime_object_plugin_test flow_default runtime_object_header)
        target_compile_options(${target} PRIVATE -fno-rtti)
    endforeach()
endif()
