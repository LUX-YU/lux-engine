generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_BINDING_NATIVE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_BINDING_NATIVE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_BINDING_NATIVE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/script_binding/native/visibility.h
)
add_component(
    COMPONENT_NAME simulation_script_binding_native
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script_binding_native
    SOURCE_FILES src/NativeScriptBindingBackend.cpp
)
component_include_directories(
    simulation_script_binding_native
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script_binding_native
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_BINDING_NATIVE_LIBRARY
)
target_link_libraries(
    simulation_script_binding_native
    PUBLIC
        lux::engine::simulation::simulation_script_binding
        lux::engine::function::script_native
)
component_add_transitive_commands(
    simulation_script_binding_native
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_binding)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_native)"
)
lux_classify_target(
    TARGET  simulation_script_binding_native
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

if(TARGET script_native_fixture)
    add_executable(
        simulation_script_binding_native_test
        test/native_binding_backend_test.cpp
    )
    target_link_libraries(
        simulation_script_binding_native_test
        PRIVATE lux::engine::simulation::simulation_script_binding_native
    )
    target_compile_definitions(
        simulation_script_binding_native_test
        PRIVATE LUX_SCRIPT_NATIVE_FIXTURE="$<TARGET_FILE:script_native_fixture>"
    )
    add_dependencies(
        simulation_script_binding_native_test
        script_native_fixture
    )
    if(MSVC)
        target_compile_options(simulation_script_binding_native_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(simulation_script_binding_native_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  simulation_script_binding_native_test
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    DOMAIN
    )
    add_test(
        NAME simulation_script_binding_native_test
        COMMAND simulation_script_binding_native_test
    )
endif()

install_components(
    PROJECT_NAME lux-engine-simulation
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   simulation_script_binding_native
)
