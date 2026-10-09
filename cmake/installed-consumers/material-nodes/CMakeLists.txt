cmake_minimum_required(VERSION 3.28)

project(MaterialNodesConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-function REQUIRED COMPONENTS material_graph)
find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)

enable_testing()

set(material_test_source "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/material/test")

add_executable(material_nodes "${material_test_source}/node_catalog.cpp")

target_link_libraries(material_nodes PRIVATE lux::engine::function::material_graph)

add_library(material_nodes_plugin MODULE "${material_test_source}/node_plugin.cpp")

target_link_libraries(material_nodes_plugin PRIVATE lux::engine::function::material_graph)

add_executable(material_nodes_plugin_test "${material_test_source}/node_plugin_test.cpp")

target_link_libraries(
    material_nodes_plugin_test
    PRIVATE lux::engine::function::material_graph lux::engine::platform::dynamic_library
)

add_dependencies(material_nodes_plugin_test material_nodes_plugin)

add_test(NAME installed.material_nodes COMMAND material_nodes)

add_test(NAME installed.material_nodes_plugin COMMAND material_nodes_plugin_test $<TARGET_FILE:material_nodes_plugin>)

set(material_headers
    lux/engine/material/MaterialNodePayload.hpp
    lux/engine/material/MaterialNodeCatalog.hpp
    lux/engine/function/graph/GraphNodeTypeIdentity.hpp
)

set(material_targets material_nodes material_nodes_plugin material_nodes_plugin_test)

foreach(header IN LISTS material_headers)
    get_filename_component(name "${header}" NAME_WE)

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp" "#include <${header}>\n")

    add_library(${name}_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp")

    target_link_libraries(${name}_header PRIVATE lux::engine::function::material_graph)

    list(APPEND material_targets ${name}_header)
endforeach()

foreach(target IN LISTS material_targets)
    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endforeach()
