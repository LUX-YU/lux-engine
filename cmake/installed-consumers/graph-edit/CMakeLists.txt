cmake_minimum_required(VERSION 3.28)

project(GraphEditConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-function REQUIRED COMPONENTS material_graph flowforge)

enable_testing()

add_executable(domain_edit "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/graph/test/domain_edit.cpp")

target_link_libraries(domain_edit PRIVATE lux::engine::function::material_graph lux::engine::function::flowforge)

if(MSVC)
    target_compile_options(domain_edit PRIVATE /permissive- /GR- /utf-8)
else()
    target_compile_options(domain_edit PRIVATE -fno-rtti)
endif()

add_test(NAME installed.graph_edit_material COMMAND domain_edit material)

add_test(NAME installed.graph_edit_flow COMMAND domain_edit flow)

add_executable(graph_edit "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/graph/test/edit.cpp")

target_link_libraries(graph_edit PRIVATE lux::engine::function::graph)

add_test(NAME installed.graph_edit COMMAND graph_edit)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "#include <lux/engine/function/graph/GraphEdit.hpp>\n")

add_library(graph_edit_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")

target_link_libraries(graph_edit_header PRIVATE lux::engine::function::graph)

option(GRAPH_EDIT_COMPILERS "Exercise the actual installed Material and Flow compilers" OFF)

if(GRAPH_EDIT_COMPILERS)
    find_package(lux-engine-toolchain-material REQUIRED COMPONENTS toolchain_material_compiler)

    find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)

    add_executable(compiler_edit "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/graph/test/compiler_edit.cpp")

    target_link_libraries(
        compiler_edit
        PRIVATE lux::engine::toolchain::toolchain_material_compiler lux::engine::flowforge::flowforge_compiler
    )

    add_test(NAME installed.graph_edit_compilers COMMAND compiler_edit)
endif()

foreach(target graph_edit graph_edit_header compiler_edit)
    if(TARGET ${target})
        if(MSVC)
            target_compile_options(${target} PRIVATE /permissive- /GR- /utf-8)
        else()
            target_compile_options(${target} PRIVATE -fno-rtti)
        endif()
    endif()
endforeach()
