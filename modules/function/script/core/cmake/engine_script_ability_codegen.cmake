include_guard(GLOBAL)

if(NOT COMMAND lux_add_codegen_job)
    find_package(lux-cxx REQUIRED CONFIG COMPONENTS reflection_generator)
    include_component_cmake_scripts(lux::cxx::reflection_generator)
endif()

function(lux_script_abilities)
    set(one_value_args TARGET OUTPUT_ROOT)
    set(multi_value_args SOURCES LOGICAL_PATHS EXTRA_COMPILE_OPTIONS)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT ARGS_TARGET OR NOT ARGS_SOURCES)
        message(FATAL_ERROR "[lux_script_abilities] TARGET and explicit SOURCES are required")
    endif()
    if(NOT TARGET ${ARGS_TARGET})
        message(FATAL_ERROR "[lux_script_abilities] unknown TARGET '${ARGS_TARGET}'")
    endif()

    if(NOT ARGS_LOGICAL_PATHS)
        foreach(source IN LISTS ARGS_SOURCES)
            get_filename_component(logical "${source}" NAME)
            list(APPEND ARGS_LOGICAL_PATHS "${logical}")
        endforeach()
    endif()
    list(LENGTH ARGS_SOURCES source_count)
    list(LENGTH ARGS_LOGICAL_PATHS logical_count)
    if(NOT source_count EQUAL logical_count)
        message(FATAL_ERROR "[lux_script_abilities] SOURCES and LOGICAL_PATHS must have equal length")
    endif()

    if(ARGS_OUTPUT_ROOT)
        set(output_root "${ARGS_OUTPUT_ROOT}")
    else()
        set(output_root "${CMAKE_CURRENT_BINARY_DIR}/generated/${ARGS_TARGET}/script_abilities")
    endif()
    set(schema_writer_root "${output_root}/schema_writer")
    set(schema_root "${output_root}/schema")
    set(job "${ARGS_TARGET}_script_abilities")
    if(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template/script_ability.template")
        set(template_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template")
    else()
        get_filename_component(package_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" DIRECTORY)
        set(template_dir "${package_dir}/template")
    endif()

    lux_add_codegen_job(
        NAME ${job}
        MARKER luxability
        TARGET_FILES ${ARGS_SOURCES}
        LOGICAL_PATHS ${ARGS_LOGICAL_PATHS}
        EXTRA_COMPILE_OPTIONS -D__LUX_PARSE_TIME__=1 ${ARGS_EXTRA_COMPILE_OPTIONS}
    )
    lux_codegen_add_validation(
        JOB ${job}
        NAME script_ability_semantics
        TEMPLATE ${template_dir}/script_ability.validation.template
    )
    lux_codegen_add_projection(
        JOB ${job}
        NAME script_ability_cpp
        TEMPLATE ${template_dir}/script_ability.template
        OUTPUT_ROOT ${output_root}
        OUTPUT_SUFFIX .ability.generated.hpp
        FLAT_OUTPUT
    )
    lux_codegen_add_projection(
        JOB ${job}
        NAME script_ability_schema_writer
        TEMPLATE ${template_dir}/script_ability_schema.template
        OUTPUT_ROOT ${schema_writer_root}
        OUTPUT_SUFFIX .ability.schema.cpp
        FLAT_OUTPUT
    )
    lux_target_add_codegen(
        TARGET ${ARGS_TARGET}
        JOB ${job}
        PROJECTIONS script_ability_semantics script_ability_cpp script_ability_schema_writer
        DONT_ADD_TO_SOURCE
    )
    target_include_directories(${ARGS_TARGET} PUBLIC "$<BUILD_INTERFACE:${output_root}>")
    set(schema_writers)
    set(schema_files)
    foreach(logical IN LISTS ARGS_LOGICAL_PATHS)
        get_filename_component(schema_name "${logical}" NAME_WE)
        list(APPEND schema_writers "${schema_writer_root}/${schema_name}.ability.schema.cpp")
        list(APPEND schema_files "${schema_root}/${schema_name}.ability.schema.json")
    endforeach()
    set_target_properties(${ARGS_TARGET} PROPERTIES
        LUX_SCRIPT_ABILITY_JOB "${job}"
        LUX_SCRIPT_ABILITY_GENERATED_DIR "${output_root}"
        LUX_SCRIPT_ABILITY_SCHEMA_WRITERS "${schema_writers}"
        LUX_SCRIPT_ABILITY_SCHEMA_FILES "${schema_files}"
        LUX_SCRIPT_ABILITY_SCHEMA_TARGET ""
    )
endfunction()

function(lux_materialize_script_ability_schemas)
    cmake_parse_arguments(ARGS "" "TARGET" "" ${ARGN})
    if(NOT ARGS_TARGET OR NOT TARGET ${ARGS_TARGET})
        message(FATAL_ERROR "[lux_materialize_script_ability_schemas] valid TARGET is required")
    endif()
    get_target_property(existing ${ARGS_TARGET} LUX_SCRIPT_ABILITY_SCHEMA_TARGET)
    if(existing)
        return()
    endif()
    get_target_property(job ${ARGS_TARGET} LUX_SCRIPT_ABILITY_JOB)
    get_target_property(writers ${ARGS_TARGET} LUX_SCRIPT_ABILITY_SCHEMA_WRITERS)
    get_target_property(schemas ${ARGS_TARGET} LUX_SCRIPT_ABILITY_SCHEMA_FILES)
    if(NOT job OR NOT writers OR NOT schemas)
        message(FATAL_ERROR
            "[lux_materialize_script_ability_schemas] TARGET must first call lux_script_abilities"
        )
    endif()
    list(LENGTH writers writer_count)
    list(LENGTH schemas schema_count)
    if(NOT writer_count EQUAL schema_count)
        message(FATAL_ERROR "[lux_materialize_script_ability_schemas] writer/schema count mismatch")
    endif()

    string(REPLACE "::" "_" target_stem "${ARGS_TARGET}")
    set(generated_schemas)
    math(EXPR last_writer "${writer_count} - 1")
    foreach(index RANGE 0 ${last_writer})
        list(GET writers ${index} writer)
        list(GET schemas ${index} schema)
        get_filename_component(schema_directory "${schema}" DIRECTORY)
        set(exporter "${target_stem}_script_ability_schema_${index}")
        set_source_files_properties("${writer}" PROPERTIES GENERATED TRUE)
        add_executable(${exporter} EXCLUDE_FROM_ALL "${writer}")
        target_link_libraries(${exporter} PRIVATE ${ARGS_TARGET})
        add_dependencies(${exporter} "${job}_generate")
        if(COMMAND lux_classify_target)
            lux_classify_target(
                TARGET  ${exporter}
                LAYER   TOOLCHAIN
                PRODUCT BUILD_TOOL
                ROLE    DOMAIN
            )
        endif()
        add_custom_command(
            OUTPUT "${schema}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${schema_directory}"
            COMMAND $<TARGET_FILE:${exporter}> "${schema}"
            DEPENDS ${exporter} "${writer}"
            VERBATIM
        )
        list(APPEND generated_schemas "${schema}")
    endforeach()
    set(schema_target "${target_stem}_script_ability_schemas")
    add_custom_target(${schema_target} DEPENDS ${generated_schemas})
    if(COMMAND lux_classify_target)
        lux_classify_target(
            TARGET  ${schema_target}
            LAYER   TOOLCHAIN
            PRODUCT BUILD_TOOL
            ROLE    DOMAIN
        )
    endif()
    set_target_properties(${ARGS_TARGET} PROPERTIES
        LUX_SCRIPT_ABILITY_SCHEMA_TARGET "${schema_target}"
        LUX_SCRIPT_ABILITY_SCHEMA_FILES "${generated_schemas}"
    )
endfunction()
