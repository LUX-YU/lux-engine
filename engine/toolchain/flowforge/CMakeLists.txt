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
        ${PROJECT_SOURCE_DIR}/modules/function/flowforge/sinclude
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

lux_engine_install_components(
    PROJECT_NAME lux-engine-flowforge-compiler
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::flowforge
    COMPONENTS   flowforge_compiler
)

if(BUILD_TESTING AND LUX_EDITOR_BUILD_TOOLCHAIN_TESTS)
    add_executable(flowforge_ability_contract_test test/ability_contract.cpp)
    target_link_libraries(flowforge_ability_contract_test PRIVATE flowforge_compiler)
    lux_editor_test_options(flowforge_ability_contract_test)
    add_test(NAME flowforge.ability_contract COMMAND flowforge_ability_contract_test)

    add_executable(flowforge_scalar_lowering_test test/scalar_lowering.cpp)

    target_link_libraries(flowforge_scalar_lowering_test PRIVATE flowforge_compiler)

    lux_editor_test_options(flowforge_scalar_lowering_test)

    add_test(NAME flowforge.scalar_lowering COMMAND flowforge_scalar_lowering_test)

    add_executable(flowforge_nested_control_test test/nested_control.cpp)

    target_link_libraries(flowforge_nested_control_test PRIVATE flowforge_compiler script_native)

    lux_editor_test_options(flowforge_nested_control_test)

    add_test(NAME flowforge.nested_control COMMAND flowforge_nested_control_test)
endif()

if(BUILD_TESTING AND LUX_EDITOR_BUILD_TOOLCHAIN_TESTS)
    add_executable(flowforge_native_definition_test test/native_definition.cpp)

    target_link_libraries(flowforge_native_definition_test PRIVATE flowforge_compiler)

    lux_editor_test_options(flowforge_native_definition_test)

    add_test(NAME flowforge.native_definition COMMAND flowforge_native_definition_test)
endif()

if(BUILD_TESTING AND LUX_EDITOR_BUILD_TOOLCHAIN_TESTS)
    add_executable(flowforge_analysis_test ${PROJECT_SOURCE_DIR}/modules/function/flowforge/test/analysis.cpp)

    target_link_libraries(flowforge_analysis_test PRIVATE flowforge_compiler)

    target_compile_definitions(flowforge_analysis_test PRIVATE FLOW_ANALYSIS_COMPILER=1)

    lux_editor_test_options(flowforge_analysis_test)

    add_test(NAME flowforge.analysis COMMAND flowforge_analysis_test)
endif()

if(BUILD_TESTING)
    add_executable(flowforge_control_lowering_test test/control_lowering.cpp)

    target_link_libraries(flowforge_control_lowering_test PRIVATE lux::engine::flowforge::flowforge_compiler)

    add_test(NAME flowforge.control_lowering_compatibility COMMAND flowforge_control_lowering_test)
endif()
