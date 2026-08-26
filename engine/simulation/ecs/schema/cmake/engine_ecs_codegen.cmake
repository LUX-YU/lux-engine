function(engine_target_add_ecs_component_codegen)
    set(one_value_args NAME TARGET HEADER LOGICAL_PATH SYMBOL SOURCE_FILE)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "" ${ARGN})
    if(NOT ARGS_NAME OR NOT ARGS_TARGET OR NOT ARGS_HEADER OR
       NOT ARGS_LOGICAL_PATH OR NOT ARGS_SYMBOL)
        message(FATAL_ERROR
            "[engine_target_add_ecs_component_codegen] NAME, TARGET, HEADER, LOGICAL_PATH and SYMBOL are required"
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
    lux_codegen_add_validation(
        JOB ${ARGS_NAME}
        NAME ecs_component_semantics
        TEMPLATE ${PROJECT_SOURCE_DIR}/engine/simulation/ecs/schema/template/ecs_component.validation.template
    )
    lux_codegen_add_projection(
        JOB ${ARGS_NAME}
        NAME type_static_info
        TEMPLATE ${PROJECT_SOURCE_DIR}/modules/core/meta/template/type_static_info.template
        OUTPUT_ROOT ${LUX_GENERATE_HEADER_DIR}
        OUTPUT_SUFFIX .type_static_info.hpp
    )
    lux_codegen_add_projection(
        JOB ${ARGS_NAME}
        NAME ecs_schema
        TEMPLATE ${PROJECT_SOURCE_DIR}/engine/simulation/ecs/schema/template/ecs_schema.template
        OUTPUT_ROOT ${LUX_GENERATE_HEADER_DIR}
        OUTPUT_SUFFIX .ecs_schema.hpp
        JSON_FIELD "{\"projection_symbol\":\"${ARGS_SYMBOL}\"}"
    )
    lux_codegen_add_projection(
        JOB ${ARGS_NAME}
        NAME ecs_snapshot
        TEMPLATE ${PROJECT_SOURCE_DIR}/engine/simulation/ecs/schema/template/ecs_snapshot.template
        OUTPUT_ROOT ${LUX_GENERATE_HEADER_DIR}
        OUTPUT_SUFFIX .ecs_snapshot.hpp
        JSON_FIELD "{\"projection_symbol\":\"${ARGS_SYMBOL}\"}"
    )
    lux_target_add_codegen(
        TARGET ${ARGS_TARGET}
        JOB ${ARGS_NAME}
        DONT_ADD_TO_SOURCE
    )
endfunction()
