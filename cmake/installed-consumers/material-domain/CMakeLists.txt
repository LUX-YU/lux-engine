cmake_minimum_required(VERSION 3.28)

project(MaterialDomainConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MATERIAL_DOMAIN_COMPILER "Also consume the actual toolchain compiler" OFF)

find_package(lux-engine-function REQUIRED COMPONENTS material_graph)

enable_testing()

add_executable(material_shader_ir "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/material/test/shader_ir.cpp")

target_link_libraries(material_shader_ir PRIVATE lux::engine::function::material_graph)

add_test(NAME installed.material_shader_ir COMMAND material_shader_ir)

add_executable(
    material_node_identity
    "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/material/test/node_identity.cpp"
)

target_link_libraries(material_node_identity PRIVATE lux::engine::function::material_graph)

add_test(NAME installed.material_node_identity COMMAND material_node_identity)

foreach(header ShaderIR MaterialCompileFailure graph/Node graph/MaterialGraph graph/MaterialSource)
    string(REPLACE "/" "_" target_name "${header}")

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.cpp" "#include <lux/engine/material/${header}.hpp>\n")

    add_library(${target_name}_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.cpp")

    target_link_libraries(${target_name}_header PRIVATE lux::engine::function::material_graph)
endforeach()

foreach(target
    material_shader_ir
    material_node_identity
    ShaderIR_header
    MaterialCompileFailure_header
    graph_Node_header
    graph_MaterialGraph_header
    graph_MaterialSource_header
)
    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endforeach()

if(MATERIAL_DOMAIN_COMPILER)
    find_package(lux-engine-toolchain-material REQUIRED COMPONENTS toolchain_material_compiler)

    add_executable(
        material_domain_compiler
        "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/material/test/domain_boundary.cpp"
    )

    target_link_libraries(material_domain_compiler PRIVATE lux::engine::toolchain::toolchain_material_compiler)

    if(MSVC)
        target_compile_options(material_domain_compiler PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(material_domain_compiler PRIVATE -fno-rtti)
    endif()

    add_test(
        NAME installed.material_domain_compiler
        COMMAND material_domain_compiler "${CMAKE_CURRENT_BINARY_DIR}/material.spirv.bin"
    )
endif()
