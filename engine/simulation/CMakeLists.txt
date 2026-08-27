add_subdirectory(description)
add_subdirectory(asset)
add_subdirectory(ecs/core)
add_subdirectory(ecs/schema)
add_subdirectory(ecs/snapshot)
add_subdirectory(system)
add_subdirectory(script_binding)
add_subdirectory(ecs/task)
add_subdirectory(ecs/hierarchy)
add_subdirectory(ecs/transform)
add_subdirectory(ecs)

set(LUX_SIMULATION_COMPONENTS
    simulation_description
    simulation_script_binding
    simulation_script_binding_cpp_static
)
if(TARGET simulation_script_binding_native)
    list(APPEND
        LUX_SIMULATION_COMPONENTS
        simulation_script_binding_native
    )
endif()
if(TARGET simulation_script_binding_lua)
    list(APPEND
        LUX_SIMULATION_COMPONENTS
        simulation_script_binding_lua
    )
endif()
install_components(
    PROJECT_NAME lux-engine-simulation
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   ${LUX_SIMULATION_COMPONENTS}
)
