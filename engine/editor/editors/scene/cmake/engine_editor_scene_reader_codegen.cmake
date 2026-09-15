function(engine_target_add_scene_reader_codegen)
    set(one_value_args NAME TARGET HEADER LOGICAL_PATH SYMBOL SOURCE_FILE OUTPUT_ROOT)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "" ${ARGN})
    if(NOT ARGS_NAME OR NOT ARGS_TARGET OR NOT ARGS_HEADER OR NOT ARGS_LOGICAL_PATH OR NOT ARGS_SYMBOL)
        message(FATAL_ERROR
            "[engine_target_add_scene_reader_codegen] NAME, TARGET, HEADER, LOGICAL_PATH and SYMBOL are required"
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
    if(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../template/scene_reader.validation.template")
        set(_lux_scene_reader_template_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../template")
    else()
        set(_lux_scene_reader_template_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template")
    endif()
    lux_codegen_add_validation(
        JOB ${ARGS_NAME}
        NAME scene_reader_semantics
        TEMPLATE ${_lux_scene_reader_template_dir}/scene_reader.validation.template
    )
    if(ARGS_OUTPUT_ROOT)
        set(_lux_scene_reader_output_root "${ARGS_OUTPUT_ROOT}")
    else()
        set(_lux_scene_reader_output_root "${CMAKE_CURRENT_BINARY_DIR}/scene_reader_gen")
    endif()
    lux_codegen_add_projection(
        JOB ${ARGS_NAME}
        NAME scene_reader
        TEMPLATE ${_lux_scene_reader_template_dir}/scene_reader.template
        OUTPUT_ROOT ${_lux_scene_reader_output_root}
        OUTPUT_SUFFIX .scene_reader.hpp
        JSON_FIELD "{\"projection_symbol\":\"${ARGS_SYMBOL}\"}"
    )
    lux_target_add_codegen(
        TARGET ${ARGS_TARGET}
        JOB ${ARGS_NAME}
        DONT_ADD_TO_SOURCE
    )
    target_include_directories(${ARGS_TARGET} PRIVATE ${_lux_scene_reader_output_root})
endfunction()
