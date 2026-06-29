## engine::flowforge_compiler
##
## Editor / dev-time compiler that lowers FlowForge graphs through MLIR / LLVM
## into runtime artefacts consumed by `lux::script::ScriptHost`.
##
## This is the ONLY target in the engine that drags in MLIR / LLVM and is
## opt-in via `LUX_ENABLE_FLOWFORGE_MLIR`. It belongs to the editor tier
## (engine/) because it is editor-side / build-time, never linked into shipped
## game runtimes.
##
## Build target: `lux::engine::flowforge_compiler`

option(LUX_ENABLE_FLOWFORGE_MLIR
       "Build the FlowForge MLIR compiler (dialect, IR, ExecutionEngine)" OFF)

if(NOT LUX_ENABLE_FLOWFORGE_MLIR)
    return()
endif()

# FLOWFORGE_MODULE_DIR points at the graph-model component (still in the
# function layer) so the in-tree dialect target keeps working without
# rewriting its private includes.
set(FLOWFORGE_MODULE_DIR ${CMAKE_SOURCE_DIR}/modules/function/flowforge)

add_subdirectory(dialect)

set(FLOWFORGE_COMPILER_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/IR.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Passes.cpp
)

add_component(
    COMPONENT_NAME  flowforge_compiler
    NAMESPACE       lux::engine
    OUTPUT_NAME     lux_engine_flowforge_compiler
    SOURCE_FILES    ${FLOWFORGE_COMPILER_SRCS}
)

component_include_directories(
    flowforge_compiler
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
        ${MLIR_INCLUDE_DIRS}
    PRIVATE
        pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/dialect
        ${CMAKE_CURRENT_BINARY_DIR}/dialect
    INSTALL_TIME
        include
)

target_compile_definitions(
    flowforge_compiler
    PRIVATE
    LUX_EDITOR_LIBRARY
    PUBLIC
    LUX_FLOWFORGE_HAS_MLIR=1
)

add_dependencies(
    flowforge_compiler
    FlowForgeDialectIncGen
)

target_link_libraries(
    flowforge_compiler
    PUBLIC
    lux::engine::function::flowforge
    lux::engine::core::script
    PRIVATE
    MLIRFlowForgeDialect
)

component_add_transitive_commands(
    flowforge_compiler
    "find_package(lux-engine-function REQUIRED COMPONENTS flowforge)"
    "find_package(lux-engine-core     REQUIRED COMPONENTS script)"
    "find_package(MLIR                 REQUIRED CONFIG)"
)

set(ENABLE_FLOWFORGE_TEST OFF CACHE BOOL "Enable FlowForge MLIR test")
if(ENABLE_FLOWFORGE_TEST)
    add_subdirectory(test)
endif()
