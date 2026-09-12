generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/scripting/native/visibility.h
)
add_component(
    COMPONENT_NAME simulation_script_native
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script_native
    SOURCE_FILES src/NativeScriptBackend.cpp
)
component_include_directories(
    simulation_script_native
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(simulation_script_native PRIVATE pinclude)
target_compile_definitions(
    simulation_script_native
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_LIBRARY
)
target_link_libraries(
    simulation_script_native
    PUBLIC
        lux::engine::simulation::simulation_script
        lux::engine::function::script_native
)
component_add_transitive_commands(
    simulation_script_native
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_native)"
)
