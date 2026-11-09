set(engine_render_default OFF)
if(TARGET lux::engine::function::render)
    set(engine_render_default ON)
endif()

option(LUX_ENGINE_WITH_RENDER "Initialize rendering with engine::init" ${engine_render_default})

add_subdirectory(foundation)
add_subdirectory(domain)
add_subdirectory(services)
add_subdirectory(runtime)
add_subdirectory(systems)

if(LUX_ENGINE_WITH_RENDER)
    if(NOT TARGET system_render)
        message(FATAL_ERROR "LUX_ENGINE_WITH_RENDER requires the Render module")
    endif()

    target_compile_definitions(engine_context INTERFACE LUX_ENGINE_WITH_RENDER=1)

endif()

add_subdirectory(runtime/initialization)
add_subdirectory(runtime/plugins)

if(LUX_PROFILE_HAS_TOOLCHAIN)
    add_subdirectory(toolchain)
elseif(BUILD_TESTING)
    # Developer validation compiles the actual Scene toolchain without loading all optional toolchains.
    add_subdirectory(toolchain/scene)
endif()

if(BUILD_TESTING AND LUX_ENGINE_WITH_RENDER AND TARGET window AND TARGET ui)
    add_subdirectory(test)
endif()

set(_lux_ecs_components)
foreach(component IN ITEMS
    ecs_core
    ecs_schema
    ecs_snapshot
    ecs_hierarchy
    ecs_transform
    ecs_visual
)
    if(TARGET ${component})
        list(APPEND _lux_ecs_components ${component})
    endif()
endforeach()

if(_lux_ecs_components)
    lux_engine_install_components(
        PROJECT_NAME lux-engine-ecs
        VERSION      ${PROJECT_VERSION}
        NAMESPACE    lux::engine::ecs
        COMPONENTS   ${_lux_ecs_components}
    )
endif()

set(LUX_SIMULATION_COMPONENTS
    simulation_description
    simulation_task
    simulation_script
    simulation_scripting_core
    simulation_script_cpp_static
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
if(TARGET simulation_script_native_lua_tasks)
    list(APPEND LUX_SIMULATION_COMPONENTS simulation_script_native_lua_tasks)
endif()
lux_engine_install_components(
    PROJECT_NAME lux-engine-simulation
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   ${LUX_SIMULATION_COMPONENTS}
)
