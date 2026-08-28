generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_FLOWFORGE_COMPILER_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_FLOWFORGE_COMPILER_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_FLOWFORGE_COMPILER_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/flowforge/compiler/visibility.h
)

add_component(
    COMPONENT_NAME flowforge_compiler
    NAMESPACE      lux::engine::flowforge
    OUTPUT_NAME    lux_engine_flowforge_compiler
    SOURCE_FILES
        src/Compiler.cpp
        src/IR.cpp
        src/IRContext.cpp
        src/Passes.cpp
        src/ScriptInstance.cpp
        src/AOT.cpp
)
component_include_directories(
    flowforge_compiler
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    flowforge_compiler
    PRIVATE LUX_ENGINE_FLOWFORGE_COMPILER_LIBRARY
)
target_link_libraries(
    flowforge_compiler
    PUBLIC
        lux::engine::function::flowforge
        lux::engine::function::script_artifact
)

find_package(LLVM CONFIG REQUIRED)
find_package(MLIR CONFIG REQUIRED)
set(FLOWFORGE_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(src/mlir)
target_include_directories(
    flowforge_compiler
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mlir
        ${CMAKE_CURRENT_BINARY_DIR}/src/mlir
        ${MLIR_INCLUDE_DIRS}
)
add_dependencies(flowforge_compiler FlowForgeDialectIncGen)
target_link_libraries(flowforge_compiler PRIVATE _flowforge_dialect)
component_add_transitive_commands(
    flowforge_compiler
    "find_package(lux-engine-function REQUIRED COMPONENTS flowforge script_artifact)"
)
lux_classify_target(
    TARGET  flowforge_compiler
    LAYER   TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

add_executable(
    flowforge_script_artifact_test
    test/script_artifact_compiler_test.cpp
)
target_link_libraries(
    flowforge_script_artifact_test
    PRIVATE
        lux::engine::flowforge::flowforge_compiler
        lux::engine::function::script_native
)
if(MSVC)
    target_compile_options(flowforge_script_artifact_test PRIVATE /UNDEBUG)
else()
    target_compile_options(flowforge_script_artifact_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  flowforge_script_artifact_test
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME flowforge_script_artifact_test
    COMMAND flowforge_script_artifact_test
)

install_components(
    PROJECT_NAME lux-engine-flowforge-compiler
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::flowforge
    COMPONENTS   flowforge_compiler
)
