include_guard(GLOBAL)

function(lux_package_lua_script)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package_lua_script.py")
        set(packager "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package_lua_script.py")
    else()
        get_filename_component(packager_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" DIRECTORY)
        set(packager "${packager_root}/package_lua_script.py")
    endif()
    set(one_value_args NAME SOURCE OUTPUT MODULE ENTRY SCOPE SYMBOL_LEDGER OUT_VAR)
    set(multi_value_args
        ABILITY_TARGETS
        ABILITY_SCHEMAS
        EVENT_SCHEMA_TARGETS
        EVENT_SCHEMAS
        SEMANTIC_TYPES
        SEMANTIC_HEADERS
        SEMANTIC_TARGETS
        SEMANTIC_INCLUDE_DIRECTORIES
        SEMANTIC_SCHEMAS
    )
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})
    foreach(required NAME SOURCE OUTPUT MODULE ENTRY SCOPE SYMBOL_LEDGER)
        if(NOT ARGS_${required})
            message(FATAL_ERROR "[lux_package_lua_script] ${required} is required")
        endif()
    endforeach()
    if(TARGET ${ARGS_NAME})
        message(FATAL_ERROR "[lux_package_lua_script] target '${ARGS_NAME}' already exists")
    endif()
    if(NOT ARGS_SCOPE STREQUAL "ENTITY" AND NOT ARGS_SCOPE STREQUAL "SIMULATION")
        message(FATAL_ERROR "[lux_package_lua_script] SCOPE must be ENTITY or SIMULATION")
    endif()

    get_filename_component(source "${ARGS_SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(output "${ARGS_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_filename_component(ledger "${ARGS_SYMBOL_LEDGER}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(schema_files ${ARGS_ABILITY_SCHEMAS})
    set(schema_targets)
    foreach(ability_target IN LISTS ARGS_ABILITY_TARGETS)
        if(NOT TARGET ${ability_target})
            message(FATAL_ERROR "[lux_package_lua_script] unknown Ability target '${ability_target}'")
        endif()
        lux_materialize_script_ability_schemas(TARGET ${ability_target})
        get_target_property(target_schemas ${ability_target} LUX_SCRIPT_ABILITY_SCHEMA_FILES)
        get_target_property(target_schema_target ${ability_target} LUX_SCRIPT_ABILITY_SCHEMA_TARGET)
        list(APPEND schema_files ${target_schemas})
        list(APPEND schema_targets ${target_schema_target})
    endforeach()
    list(REMOVE_DUPLICATES schema_files)
    list(REMOVE_DUPLICATES schema_targets)
    set(event_schema_files ${ARGS_EVENT_SCHEMAS})
    set(event_schema_targets ${ARGS_EVENT_SCHEMA_TARGETS})
    foreach(event_schema_target IN LISTS event_schema_targets)
        if(NOT TARGET ${event_schema_target})
            message(FATAL_ERROR "[lux_package_lua_script] unknown Event schema target '${event_schema_target}'")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES event_schema_files)
    list(REMOVE_DUPLICATES event_schema_targets)

    if(NOT COMMAND lux_script_semantic_schema)
        include_component_cmake_scripts(lux::engine::function::script_core)
    endif()
    set(semantic_schema "${CMAKE_CURRENT_BINARY_DIR}/${ARGS_NAME}.semantic.json")
    lux_script_semantic_schema(NAME ${ARGS_NAME}_semantics OUTPUT "${semantic_schema}"
        TYPES ${ARGS_SEMANTIC_TYPES} HEADERS ${ARGS_SEMANTIC_HEADERS} TARGETS ${ARGS_SEMANTIC_TARGETS}
        INCLUDE_DIRECTORIES ${ARGS_SEMANTIC_INCLUDE_DIRECTORIES})

    set(command
        ${Python3_EXECUTABLE}
        ${packager}
        --source ${source}
        --output ${output}
        --symbol-ledger ${ledger}
        --module ${ARGS_MODULE}
        --entry ${ARGS_ENTRY}
        --scope ${ARGS_SCOPE}
        --semantic-schema ${semantic_schema}
    )
    foreach(schema IN LISTS schema_files)
        list(APPEND command --ability-schema ${schema})
    endforeach()
    foreach(schema IN LISTS event_schema_files)
        list(APPEND command --event-schema ${schema})
    endforeach()
    foreach(schema IN LISTS ARGS_SEMANTIC_SCHEMAS)
        list(APPEND command --semantic-schema ${schema})
    endforeach()

    add_custom_command(
        OUTPUT ${output}
        COMMAND ${command}
        DEPENDS ${packager} ${source} ${ledger} ${schema_files} ${event_schema_files}
            ${semantic_schema} ${ARGS_SEMANTIC_SCHEMAS}
        VERBATIM
    )
    add_custom_target(${ARGS_NAME} DEPENDS ${output})
    add_dependencies(${ARGS_NAME} ${ARGS_NAME}_semantics)
    if(schema_targets)
        add_dependencies(${ARGS_NAME} ${schema_targets})
    endif()
    if(event_schema_targets)
        add_dependencies(${ARGS_NAME} ${event_schema_targets})
    endif()
    if(COMMAND lux_classify_target)
        lux_classify_target(
            TARGET  ${ARGS_NAME}
            LAYER   TOOLCHAIN
            PRODUCT BUILD_TOOL
            ROLE    DOMAIN
        )
    endif()
    set_target_properties(${ARGS_NAME} PROPERTIES LUX_LUA_SCRIPT_ARTIFACT "${output}")
    if(ARGS_OUT_VAR)
        set(${ARGS_OUT_VAR} "${output}" PARENT_SCOPE)
    endif()
endfunction()
