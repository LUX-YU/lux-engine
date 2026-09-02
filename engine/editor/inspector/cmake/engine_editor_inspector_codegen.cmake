function(engine_target_add_component_editor_codegen)
    set(one_value_args NAME TARGET HEADER LOGICAL_PATH SYMBOL SOURCE_FILE OUTPUT_ROOT)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "" ${ARGN})
    if(NOT ARGS_NAME OR NOT ARGS_TARGET OR NOT ARGS_HEADER OR NOT ARGS_LOGICAL_PATH OR NOT ARGS_SYMBOL)
        message(FATAL_ERROR
            "[engine_target_add_component_editor_codegen] NAME, TARGET, HEADER, LOGICAL_PATH and SYMBOL are required"
        )
    endif()

    lux_add_codegen_job(
        NAME ${ARGS_NAME}
        GENERATOR ${LUX_META_GENERATOR}
        MARKER luxref
        SOURCE_FILE ${ARGS_SOURCE_FILE}
        TARGET_FILES ${ARGS_HEADER}
        LOGICAL_PATHS ${ARGS_LOGICAL_PATH}
    )
    if(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../template/component_editor.validation.template")
        set(_lux_component_editor_template_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../template")
    else()
        set(_lux_component_editor_template_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template")
    endif()
    lux_codegen_add_validation(
        JOB ${ARGS_NAME}
        NAME component_editor_semantics
        TEMPLATE ${_lux_component_editor_template_dir}/component_editor.validation.template
    )
    if(ARGS_OUTPUT_ROOT)
        set(_lux_component_editor_output_root "${ARGS_OUTPUT_ROOT}")
    else()
        set(_lux_component_editor_output_root "${CMAKE_CURRENT_BINARY_DIR}/component_editor_gen")
    endif()
    lux_codegen_add_projection(
        JOB ${ARGS_NAME}
        NAME component_editor
        TEMPLATE ${_lux_component_editor_template_dir}/component_editor.template
        OUTPUT_ROOT ${_lux_component_editor_output_root}
        OUTPUT_SUFFIX .component_editor.hpp
        JSON_FIELD "{\"projection_symbol\":\"${ARGS_SYMBOL}\"}"
    )
    lux_target_add_codegen(
        TARGET ${ARGS_TARGET}
        JOB ${ARGS_NAME}
        DONT_ADD_TO_SOURCE
    )
    target_include_directories(${ARGS_TARGET} PRIVATE ${_lux_component_editor_output_root})
endfunction()
