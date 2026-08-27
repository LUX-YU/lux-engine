add_component(
    COMPONENT_NAME script_meta_adapter
    NAMESPACE      lux::engine::core
    OUTPUT_NAME    lux_engine_core_script_meta_adapter
    SOURCE_FILES src/ScriptMetaAdapter.cpp
)
component_include_directories(
    script_meta_adapter
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_compile_definitions(script_meta_adapter PRIVATE LUX_CORE_LIBRARY)
target_link_libraries(
    script_meta_adapter
    PUBLIC
        lux::engine::core::meta
        lux::engine::function::script_core
)
component_add_transitive_commands(
    script_meta_adapter
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
)
lux_classify_target(
    TARGET  script_meta_adapter
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    script_meta_adapter_contract_test
    test/script_meta_adapter_test.cpp
)
target_link_libraries(
    script_meta_adapter_contract_test
    PRIVATE lux::engine::core::script_meta_adapter
)
if(MSVC)
    target_compile_options(script_meta_adapter_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(script_meta_adapter_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  script_meta_adapter_contract_test
    LAYER   CORE
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME script_meta_adapter_contract_test
    COMMAND script_meta_adapter_contract_test
)

install_components(
    PROJECT_NAME lux-engine-core
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::core
    COMPONENTS   script_meta_adapter
)
