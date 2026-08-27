generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_AUTHORING_SCRIPT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_AUTHORING_SCRIPT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/authoring/script/visibility.h
)

add_component(
    COMPONENT_NAME authoring_script
    NAMESPACE      lux::engine::authoring
    OUTPUT_NAME    lux_engine_authoring_script
    SOURCE_FILES
        src/ScriptAuthoring.cpp
        src/ScriptSymbolLedger.cpp
)
component_include_directories(
    authoring_script
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    authoring_script
    PRIVATE LUX_ENGINE_AUTHORING_SCRIPT_LIBRARY
)
target_link_libraries(
    authoring_script
    PUBLIC
        lux::engine::simulation::simulation_script
        lux::engine::resource::description
)
component_add_transitive_commands(
    authoring_script
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(_authoring_script_product TOOLCHAIN)
else()
    set(_authoring_script_product EDITOR)
endif()
lux_classify_target(
    TARGET  authoring_script
    LAYER   AUTHORING
    PRODUCT ${_authoring_script_product}
    ROLE    DOMAIN
)

add_executable(authoring_script_test test/script_authoring_test.cpp)
target_link_libraries(
    authoring_script_test
    PRIVATE lux::engine::authoring::authoring_script
)
if(MSVC)
    target_compile_options(authoring_script_test PRIVATE /UNDEBUG)
else()
    target_compile_options(authoring_script_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  authoring_script_test
    LAYER   AUTHORING
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME authoring_script_test COMMAND authoring_script_test)

add_executable(script_symbol_ledger_test test/script_symbol_ledger_test.cpp)
target_link_libraries(
    script_symbol_ledger_test
    PRIVATE lux::engine::authoring::authoring_script
)
if(MSVC)
    target_compile_options(script_symbol_ledger_test PRIVATE /UNDEBUG)
else()
    target_compile_options(script_symbol_ledger_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  script_symbol_ledger_test
    LAYER   AUTHORING
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME script_symbol_ledger_test COMMAND script_symbol_ledger_test)

install_components(
    PROJECT_NAME lux-engine-authoring
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::authoring
    COMPONENTS   authoring_script
)
