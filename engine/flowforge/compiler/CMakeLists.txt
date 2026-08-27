generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_FLOWFORGE_SCRIPT_COMPILER_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_FLOWFORGE_SCRIPT_COMPILER_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_FLOWFORGE_SCRIPT_COMPILER_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/flowforge/compiler/visibility.h
)

add_component(
    COMPONENT_NAME flowforge_script_compiler
    NAMESPACE      lux::engine::flowforge
    OUTPUT_NAME    lux_engine_flowforge_script_compiler
    SOURCE_FILES src/script/ScriptArtifactCompiler.cpp
)
component_include_directories(
    flowforge_script_compiler
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    flowforge_script_compiler
    PRIVATE LUX_ENGINE_FLOWFORGE_SCRIPT_COMPILER_LIBRARY
)
target_link_libraries(
    flowforge_script_compiler
    PUBLIC
        lux::engine::flowforge::flowforge_script
        lux::engine::authoring::authoring_script
)
component_add_transitive_commands(
    flowforge_script_compiler
    "find_package(lux-engine-flowforge REQUIRED COMPONENTS flowforge_script)"
    "find_package(lux-engine-authoring REQUIRED COMPONENTS authoring_script)"
)
lux_classify_target(
    TARGET  flowforge_script_compiler
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
    PRIVATE lux::engine::flowforge::flowforge_script_compiler
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

option(LUX_ENABLE_FLOWFORGE_MLIR
    "Build the canonical FlowForge MLIR/AOT compiler" OFF)
option(LUX_ENABLE_FLOWFORGE_COMPILER_TESTS
    "Build the canonical FlowForge MLIR/AOT tests" OFF)
if(LUX_ENABLE_FLOWFORGE_MLIR)
    find_package(LLVM CONFIG REQUIRED)
    find_package(MLIR CONFIG REQUIRED)
    set(FLOWFORGE_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_subdirectory(dialect)

    add_component(
        COMPONENT_NAME flowforge_compiler
        NAMESPACE      lux::engine::flowforge
        OUTPUT_NAME    lux_engine_flowforge_compiler
        SOURCE_FILES
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
            ${MLIR_INCLUDE_DIRS}
        PRIVATE
            pinclude
            ${CMAKE_CURRENT_SOURCE_DIR}/dialect
            ${CMAKE_CURRENT_BINARY_DIR}/dialect
        INSTALL_TIME include
    )
    target_compile_definitions(
        flowforge_compiler
        PRIVATE LUX_ENGINE_FLOWFORGE_SCRIPT_COMPILER_LIBRARY
        PUBLIC LUX_FLOWFORGE_HAS_MLIR=1
    )
    add_dependencies(flowforge_compiler FlowForgeDialectIncGen)
    target_link_libraries(
        flowforge_compiler
        PUBLIC
            lux::engine::flowforge::flowforge_graph
            lux::engine::function::script_core
        PRIVATE flowforge_compiler_dialect
    )
    component_add_transitive_commands(
        flowforge_compiler
        "find_package(lux-engine-flowforge REQUIRED COMPONENTS flowforge_graph)"
        "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
        "find_package(MLIR REQUIRED CONFIG)"
    )
    lux_classify_target(
        TARGET  flowforge_compiler
        LAYER   TOOLCHAIN
        PRODUCT TOOLCHAIN
        ROLE    DOMAIN
    )
    if(LUX_ENABLE_FLOWFORGE_COMPILER_TESTS)
        add_subdirectory(test)
    endif()
endif()
