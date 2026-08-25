function(engine_validate_ecs_component_annotations header)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${header}"
        )
    endif()
    file(READ "${header}" component_source)
    string(REGEX MATCHALL "LUX_COMPONENT_SCHEMA[ \t\r\n]*\\("
        stable_components "${component_source}"
    )
    string(REGEX MATCHALL "LUX_COMPONENT[ \t\r\n]*\\("
        internal_components "${component_source}"
    )
    string(REGEX MATCHALL
        "LUX_COMPONENT_SNAPSHOT[ \t\r\n]*\\([ \t\r\n]*(COPY|REBUILD)[ \t\r\n]*\\)"
        snapshot_policies "${component_source}"
    )
    string(REGEX MATCHALL
        "LUX_COMPONENT_WORLD_SECTION[ \t\r\n]*\\([ \t\r\n]*(LOAD|OMIT)[ \t\r\n]*\\)"
        world_section_policies "${component_source}"
    )
    list(LENGTH stable_components stable_count)
    list(LENGTH internal_components internal_count)
    list(LENGTH snapshot_policies snapshot_count)
    list(LENGTH world_section_policies world_section_count)
    math(EXPR component_count "${stable_count} + ${internal_count}")
    if(NOT snapshot_count EQUAL component_count OR
       NOT world_section_count EQUAL component_count)
        message(FATAL_ERROR
            "ECS Component declarations in '${header}' must each provide "
            "LUX_COMPONENT_SNAPSHOT(COPY|REBUILD) and "
            "LUX_COMPONENT_WORLD_SECTION(LOAD|OMIT); found ${component_count} "
            "components, ${snapshot_count} snapshot policies and "
            "${world_section_count} world-section policies"
        )
    endif()
endfunction()

if(DEFINED LUX_ECS_COMPONENT_HEADER)
    engine_validate_ecs_component_annotations(
        "${LUX_ECS_COMPONENT_HEADER}"
    )
endif()
