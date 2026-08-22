## engine::toolchain_flowforge
##
## Editor / dev-time compiler that lowers FlowForge graphs through MLIR / LLVM
## into runtime artefacts consumed by `lux::script::NativeModule`.
##
## This is the ONLY target in the engine that drags in MLIR / LLVM and is
## opt-in via `LUX_ENABLE_FLOWFORGE_MLIR`. It belongs to the editor tier
## (engine/) because it is editor-side / build-time, never linked into shipped
## game runtimes.
##
## Build target: `lux::engine::toolchain::toolchain_flowforge`

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_TOOLCHAIN_FLOWFORGE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_TOOLCHAIN_FLOWFORGE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_TOOLCHAIN_FLOWFORGE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/flowforge/visibility.h
)

option(LUX_ENABLE_FLOWFORGE_MLIR
       "Build the FlowForge MLIR compiler (dialect, IR, ExecutionEngine)" OFF)

if(NOT LUX_ENABLE_FLOWFORGE_MLIR)
    return()
endif()

# The dialect consumes the Authoring-owned editable graph vocabulary.
set(FLOWFORGE_MODULE_DIR ${CMAKE_SOURCE_DIR}/engine/authoring/flowforge)

add_subdirectory(dialect)

set(FLOWFORGE_COMPILER_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/IR.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/IRContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Passes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptInstance.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AOT.cpp
)

add_component(
    COMPONENT_NAME  toolchain_flowforge
    NAMESPACE       lux::engine::toolchain
    OUTPUT_NAME     lux_engine_toolchain_flowforge
    SOURCE_FILES    ${FLOWFORGE_COMPILER_SRCS}
)

lux_classify_target(
    TARGET toolchain_flowforge
    LAYER TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE DOMAIN
)

component_include_directories(
    toolchain_flowforge
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
    toolchain_flowforge
    PRIVATE
    LUX_TOOLCHAIN_FLOWFORGE_LIBRARY
    PUBLIC
    LUX_FLOWFORGE_HAS_MLIR=1
)

# Cook-time linker for AOT.cpp's linkSharedLibrary: bake the absolute path
# of the LLVM toolchain's lld-link (vcpkg ships it next to the MLIR libs)
# so cooking works outside a VS developer shell. Runtime fallbacks (env var
# LUX_FLOWFORGE_LINKER, PATH lookup) still apply when this is not found.
find_program(LUX_FLOWFORGE_LLD_LINK NAMES lld-link
             HINTS ${LLVM_TOOLS_BINARY_DIR})
if(LUX_FLOWFORGE_LLD_LINK)
    target_compile_definitions(
        toolchain_flowforge
        PRIVATE
        LUX_FLOWFORGE_LLD_LINK="${LUX_FLOWFORGE_LLD_LINK}"
    )
endif()

add_dependencies(
    toolchain_flowforge
    FlowForgeDialectIncGen
)

target_link_libraries(
    toolchain_flowforge
    PUBLIC
    lux::engine::authoring::authoring_flowforge
    lux::engine::function::script_core
    PRIVATE
    toolchain_flowforge_dialect
)

component_add_transitive_commands(
    toolchain_flowforge
    "find_package(lux-engine-authoring REQUIRED COMPONENTS authoring_flowforge)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(MLIR                 REQUIRED CONFIG)"
)

set(ENABLE_FLOWFORGE_TEST OFF CACHE BOOL "Enable FlowForge MLIR test")
if(ENABLE_FLOWFORGE_TEST)
    add_subdirectory(test)
endif()
