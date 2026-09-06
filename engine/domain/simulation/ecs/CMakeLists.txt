set(_lux_ecs_components)
foreach(component IN ITEMS
    core
    schema
    snapshot
    ecs_task
    hierarchy
    transform
    visual
)
    if(TARGET ${component})
        list(APPEND _lux_ecs_components ${component})
    endif()
endforeach()

if(_lux_ecs_components)
    install_components(
        PROJECT_NAME lux-engine-simulation-ecs
        VERSION      ${PROJECT_VERSION}
        NAMESPACE    lux::engine::simulation::ecs
        COMPONENTS   ${_lux_ecs_components}
    )
endif()
