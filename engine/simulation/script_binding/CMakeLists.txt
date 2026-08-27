generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_BINDING_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_BINDING_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_BINDING_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/script_binding/visibility.h
)

add_component(
    COMPONENT_NAME simulation_script_binding
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script_binding
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptBindingSession.cpp
)

component_include_directories(
    simulation_script_binding
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script_binding
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_BINDING_LIBRARY
)
target_link_libraries(
    simulation_script_binding
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::ecs::core
        lux::engine::resource::script_asset
        lux::engine::function::script_core
)
component_add_transitive_commands(
    simulation_script_binding
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS script_asset)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
)
lux_classify_target(
    TARGET  simulation_script_binding
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    simulation_script_binding_contract_test
    test/script_binding_session_test.cpp
)
target_link_libraries(
    simulation_script_binding_contract_test
    PRIVATE
        lux::engine::simulation::simulation_script_binding
        lux::engine::simulation::ecs::snapshot
)
if(MSVC)
    target_compile_options(simulation_script_binding_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_binding_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_binding_contract_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_binding_contract_test
    COMMAND simulation_script_binding_contract_test
)

install_components(
    PROJECT_NAME lux-engine-simulation
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   simulation_script_binding
)

if(TARGET script_native)
    add_subdirectory(native)
endif()
