function(engine_target_add_static_type_info)
    set(one_value_args NAME TARGET OUT_DIR SOURCE_FILE)
    set(multi_value_args HEADERS LOGICAL_PATHS EXTRA_COMPILE_OPTIONS)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT ARGS_NAME OR NOT ARGS_TARGET OR NOT ARGS_HEADERS)
        message(FATAL_ERROR
            "[engine_target_add_static_type_info] NAME, TARGET and HEADERS are required")
    endif()
    if(NOT ARGS_LOGICAL_PATHS)
        foreach(_header IN LISTS ARGS_HEADERS)
            get_filename_component(_logical "${_header}" NAME)
            list(APPEND ARGS_LOGICAL_PATHS "${_logical}")
        endforeach()
    endif()
    if(NOT ARGS_OUT_DIR)
        set(ARGS_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/type_info")
    endif()

    lux_add_codegen_job(
        NAME                  "${ARGS_NAME}"
        GENERATOR             "${LUX_META_GENERATOR}"
        MARKER                luxref
        SOURCE_FILE           "${ARGS_SOURCE_FILE}"
        TARGET_FILES          ${ARGS_HEADERS}
        LOGICAL_PATHS         ${ARGS_LOGICAL_PATHS}
        EXTRA_COMPILE_OPTIONS ${ARGS_EXTRA_COMPILE_OPTIONS}
    )
    lux_codegen_add_projection(
        JOB           "${ARGS_NAME}"
        NAME          type_static_info
        TEMPLATE      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../template/type_static_info.template"
        OUTPUT_ROOT   "${ARGS_OUT_DIR}"
        OUTPUT_SUFFIX .type_static_info.hpp
    )
    lux_target_add_codegen(
        TARGET "${ARGS_TARGET}"
        JOB    "${ARGS_NAME}"
        DONT_ADD_TO_SOURCE
    )
endfunction()
