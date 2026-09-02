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
    lux_target_add_codegen(
        TARGET ${ARGS_TARGET}
        JOB ${job}
        PROJECTIONS script_ability_semantics script_ability_cpp
        DONT_ADD_TO_SOURCE
    )
    target_include_directories(${ARGS_TARGET} PUBLIC "$<BUILD_INTERFACE:${output_root}>")
    set_target_properties(${ARGS_TARGET} PROPERTIES
        LUX_SCRIPT_ABILITY_JOB "${job}"
        LUX_SCRIPT_ABILITY_GENERATED_DIR "${output_root}"
    )
endfunction()
