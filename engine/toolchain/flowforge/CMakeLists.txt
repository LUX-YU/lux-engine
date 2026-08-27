generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_FLOWFORGE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_FLOWFORGE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TOOLCHAIN_FLOWFORGE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/flowforge/visibility.h
)
add_component(
    COMPONENT_NAME toolchain_flowforge
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_flowforge
    SOURCE_FILES src/ScriptCompiler.cpp
)
component_include_directories(
    toolchain_flowforge
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    toolchain_flowforge
    PRIVATE LUX_ENGINE_TOOLCHAIN_FLOWFORGE_LIBRARY
)
target_link_libraries(
    toolchain_flowforge
    PUBLIC
        lux::engine::authoring::authoring_flowforge
        lux::engine::resource::description
)
component_add_transitive_commands(
    toolchain_flowforge
    "find_package(lux-engine-authoring REQUIRED COMPONENTS authoring_flowforge)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)
lux_classify_target(
    TARGET  toolchain_flowforge
    LAYER   TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

if(LUX_ENABLE_FLOWFORGE_MLIR)
    find_package(MLIR CONFIG REQUIRED)
    add_library(toolchain_flowforge_mlir INTERFACE)
    target_link_libraries(toolchain_flowforge_mlir INTERFACE MLIRIR)
    lux_classify_target(
        TARGET  toolchain_flowforge_mlir
        LAYER   TOOLCHAIN
        PRODUCT BUILD_TOOL
        ROLE    DOMAIN
    )
    target_link_libraries(toolchain_flowforge PRIVATE toolchain_flowforge_mlir)
endif()

add_executable(
    flowforge_script_v3_contract_test
    test/flowforge_script_v3_test.cpp
)
target_link_libraries(
    flowforge_script_v3_contract_test
    PRIVATE
        lux::engine::toolchain::toolchain_flowforge
        lux::engine::simulation::description
)
if(MSVC)
    target_compile_options(flowforge_script_v3_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(flowforge_script_v3_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  flowforge_script_v3_contract_test
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME flowforge_script_v3_contract_test
    COMMAND flowforge_script_v3_contract_test
)

install_components(
    PROJECT_NAME lux-engine-toolchain
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::toolchain
    COMPONENTS   toolchain_flowforge
)
