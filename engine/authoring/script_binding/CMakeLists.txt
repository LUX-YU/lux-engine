generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_AUTHORING_SCRIPT_BINDING_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_AUTHORING_SCRIPT_BINDING_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/authoring/script_binding/visibility.h
)
add_component(
    COMPONENT_NAME authoring_script_binding
    NAMESPACE      lux::engine::authoring
    OUTPUT_NAME    lux_engine_authoring_script_binding
    SOURCE_FILES
        src/ScriptBindingAuthoring.cpp
        src/ScriptAuthoringSemanticCatalog.cpp
        src/ScriptSymbolLedger.cpp
)
component_include_directories(
    authoring_script_binding
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    authoring_script_binding
    PRIVATE LUX_ENGINE_AUTHORING_SCRIPT_BINDING_LIBRARY
)
target_link_libraries(
    authoring_script_binding
    PUBLIC lux::engine::simulation::description
)
component_add_transitive_commands(
    authoring_script_binding
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
)
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(_script_binding_product TOOLCHAIN)
else()
    set(_script_binding_product EDITOR)
endif()
lux_classify_target(
    TARGET  authoring_script_binding
    LAYER   AUTHORING
    PRODUCT ${_script_binding_product}
    ROLE    DOMAIN
)

add_executable(
    authoring_script_binding_test
    test/script_binding_authoring_test.cpp
)
target_link_libraries(
    authoring_script_binding_test
    PRIVATE lux::engine::authoring::authoring_script_binding
)
if(MSVC)
    target_compile_options(authoring_script_binding_test PRIVATE /UNDEBUG)
else()
    target_compile_options(authoring_script_binding_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  authoring_script_binding_test
    LAYER   AUTHORING
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME authoring_script_binding_test
    COMMAND authoring_script_binding_test
)

add_executable(
    script_symbol_ledger_test
    test/script_symbol_ledger_test.cpp
)
target_link_libraries(
    script_symbol_ledger_test
    PRIVATE lux::engine::authoring::authoring_script_binding
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
target_compile_definitions(
    authoring_script_binding_test
    PRIVATE
        LUX_SCRIPT_SEMANTIC_CATALOG="${CMAKE_CURRENT_BINARY_DIR}/lux-script-semantics-v1.json"
)

configure_file(
    assets/lux-script-semantics-v1.json
    ${CMAKE_CURRENT_BINARY_DIR}/lux-script-semantics-v1.json
    @ONLY
    NEWLINE_STYLE LF
)

install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/lux-script-semantics-v1.json
    DESTINATION share/lux-engine/script
)
