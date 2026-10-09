cmake_minimum_required(VERSION 3.28)

project(FlowAbilityDescriptionConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-function REQUIRED COMPONENTS flowforge)

find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)

enable_testing()

add_executable(
    flow_ability_description
    "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test/ability_description.cpp"
)

target_link_libraries(flow_ability_description PRIVATE lux::engine::function::flowforge)

if(MSVC)
    target_compile_options(flow_ability_description PRIVATE /permissive- /GR- /utf-8)
else()
    target_compile_options(flow_ability_description PRIVATE -fno-rtti)
endif()

add_test(NAME installed.flow_ability_catalog COMMAND flow_ability_description catalog)
add_test(NAME installed.flow_ability_node COMMAND flow_ability_description node)

set(flow_tests "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/flowforge/test")

add_library(flow_ability_plugin SHARED "${flow_tests}/ability_plugin.cpp")

target_link_libraries(flow_ability_plugin PRIVATE lux::engine::function::flowforge)

add_executable(flow_ability_plugin_test "${flow_tests}/ability_plugin_test.cpp")

target_link_libraries(
    flow_ability_plugin_test
    PRIVATE lux::engine::function::flowforge lux::engine::platform::dynamic_library
)

add_test(NAME installed.flow_ability_plugin COMMAND flow_ability_plugin_test "$<TARGET_FILE:flow_ability_plugin>")

foreach(header ScriptAbilityCatalog ScriptAbilityNode)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" "#include <lux/engine/flowforge/script/${header}.hpp>\n")

    add_library(${header}_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

    target_link_libraries(${header}_header PRIVATE lux::engine::function::flowforge)
endforeach()

foreach(target flow_ability_plugin flow_ability_plugin_test ScriptAbilityCatalog_header ScriptAbilityNode_header)
    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endforeach()
