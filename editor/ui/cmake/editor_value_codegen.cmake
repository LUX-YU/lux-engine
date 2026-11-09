include_guard(GLOBAL)

function(editor_target_add_value_codegen)
    cmake_parse_arguments(P "" "TARGET;NAME;SOURCE_FILE;HEADER;LOGICAL_PATH;POLICY" "" ${ARGN})
    set(output_root "${CMAKE_CURRENT_BINARY_DIR}/value-editors/${P_NAME}")
    string(REGEX REPLACE "\\.[^.]+$" ".value_editor.hpp" generated_header "${P_LOGICAL_PATH}")

    lux_add_codegen_job(
        NAME ${P_NAME}
        GENERATOR "${LUX_META_GENERATOR}"
        MARKER luxeditor
        SOURCE_FILE "${P_SOURCE_FILE}"
        TARGET_FILES "${P_HEADER}"
        LOGICAL_PATHS "${P_LOGICAL_PATH}"
    )

    lux_codegen_add_validation(
        JOB ${P_NAME}
        NAME editor_policy
        TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../template/value_editor.validation.template"
        JSON_FILE "editors=${P_POLICY}"
    )

    foreach(projection IN ITEMS declarations definitions)
        if(projection STREQUAL declarations)
            set(suffix .value_editor.hpp)
        else()
            set(suffix .value_editor.cpp)
        endif()
        lux_codegen_add_projection(
            JOB ${P_NAME}
            NAME ${projection}
            TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../template/value_editor.template"
            OUTPUT_ROOT "${output_root}"
            OUTPUT_SUFFIX "${suffix}"
            JSON_FIELD "{\"generated_header\":\"${generated_header}\"}"
            JSON_FILE "editors=${P_POLICY}"
        )
    endforeach()

    lux_target_add_codegen(TARGET ${P_TARGET} JOB ${P_NAME})
endfunction()
