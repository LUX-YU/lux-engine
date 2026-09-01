add_subdirectory(description)
add_subdirectory(asset)
add_subdirectory(ecs/core)
add_subdirectory(ecs/schema)
add_subdirectory(ecs/snapshot)
add_subdirectory(system)
add_subdirectory(scripting)
add_subdirectory(builtin/script)
add_subdirectory(ecs/task)
add_subdirectory(composition)
add_subdirectory(ecs/hierarchy)
add_subdirectory(ecs/transform)
add_subdirectory(ecs/visual)
add_subdirectory(builtin/transform)
add_subdirectory(ecs)

set(LUX_SIMULATION_COMPONENTS
    simulation_description
    simulation_script
    simulation_scripting_core
    simulation_script_cpp_static
    transform_system
)
if(TARGET simulation_script_native)
    list(APPEND
        LUX_SIMULATION_COMPONENTS
        simulation_script_native
    )
endif()
if(TARGET simulation_script_lua)
    list(APPEND
        LUX_SIMULATION_COMPONENTS
        simulation_script_lua
    )
endif()
install_components(
    PROJECT_NAME lux-engine-simulation
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   ${LUX_SIMULATION_COMPONENTS}
)
