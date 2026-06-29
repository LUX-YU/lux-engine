find_package(lux-cxx REQUIRED COMPONENTS algorithm container reflection_runtime)

# Phase 4 (FlowForge / script decoupling plan):
# `function::flowforge` is now the pure graph-runtime model. All MLIR / LLVM
# code (IR.hpp / IR.cpp / dialect/) has moved to the editor tier as
# `lux::engine::flowforge_compiler` (engine/flowforge_compiler/). Consumers
# that need the compiler must depend on that target explicitly and enable
# LUX_ENABLE_FLOWFORGE_MLIR.

set(FLOW_FORGE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NodeRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NodeBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ControlNode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FunctionalNode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectNode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FlowGraph.cpp
)

add_component(
    COMPONENT_NAME flowforge
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${FLOW_FORGE_SRCS}
)

target_compile_definitions(
    flowforge
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

component_include_directories(
    flowforge
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_link_libraries(
    flowforge
    PUBLIC
    lux::cxx::container
    lux::cxx::algorithm
    lux::cxx::reflection_runtime
    lux::engine::core::script
)

component_add_transitive_commands(
    flowforge
    "find_package(lux-cxx REQUIRED COMPONENTS container algorithm reflection_runtime)"
    "find_package(lux-engine-core REQUIRED COMPONENTS script)"
)

set(ENABLE_FLOWFORGE_TEST OFF CACHE BOOL "Enable FlowForge test")
if(ENABLE_FLOWFORGE_TEST)
    add_subdirectory(test)
endif()
