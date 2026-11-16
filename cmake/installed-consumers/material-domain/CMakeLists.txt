cmake_minimum_required(VERSION 3.28)

project(MaterialDomainConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-function REQUIRED COMPONENTS material_graph material_compiler)
find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)

enable_testing()

set(material_test_dir "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/material/test")

foreach(contract registered_graph authoring_schema node_identity source_generation math_registration node_catalog)
    add_executable(material_${contract} "${material_test_dir}/${contract}.cpp")

    target_link_libraries(material_${contract} PRIVATE lux::engine::function::material_compiler)

    if(MSVC)
        target_compile_options(material_${contract} PRIVATE /permissive- /GR- /utf-8)
    endif()

    add_test(
        NAME installed.material_${contract}
        COMMAND material_${contract} "${material_test_dir}/fixtures/all_nodes_v1.luxmaterial"
    )
endforeach()

add_library(material_node_plugin MODULE "${material_test_dir}/node_plugin.cpp")

target_link_libraries(material_node_plugin PRIVATE lux::engine::function::material_graph)

add_executable(material_node_plugin_test "${material_test_dir}/node_plugin_test.cpp")

target_link_libraries(
    material_node_plugin_test
    PRIVATE lux::engine::function::material_compiler lux::engine::platform::dynamic_library
)

add_dependencies(material_node_plugin_test material_node_plugin)

add_test(NAME installed.material_node_plugin COMMAND material_node_plugin_test $<TARGET_FILE:material_node_plugin>)

foreach(header
    BuiltinMaterialNodes MaterialCompileFailure MaterialMath MaterialNodeCatalog
    graph/MaterialNode graph/MaterialGraph graph/MaterialSource compiler/MaterialCompiler
)
    string(REPLACE "/" "_" target_name "${header}")

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.cpp" "#include <lux/engine/material/${header}.hpp>\n")

    add_library(${target_name}_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.cpp")

    if(header MATCHES "^compiler/")
        target_link_libraries(${target_name}_header PRIVATE lux::engine::function::material_compiler)
    else()
        target_link_libraries(${target_name}_header PRIVATE lux::engine::function::material_graph)
    endif()
endforeach()
