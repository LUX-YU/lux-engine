include_guard(GLOBAL)

# The product supplies actual declaration headers. Core generators never import
# this projection or interpret Engine schemas; one job serves each plugin input set.
function(engine_generate_plugin_value_contract)
    cmake_parse_arguments(P "" "TARGET;SOURCE_TARGET;SOURCE_FILE;OUTPUT_VARIABLE" "HEADERS" ${ARGN})
    set(job "${P_TARGET}_value_contract")
    set(output_root "${CMAKE_CURRENT_BINARY_DIR}/plugin/${P_TARGET}/value-contracts")
    set(logical_paths)
    set(outputs)
    foreach(header IN LISTS P_HEADERS)
        get_filename_component(name "${header}" NAME)
        get_filename_component(stem "${header}" NAME_WE)
        list(APPEND logical_paths "${name}")
        list(APPEND outputs "${output_root}/${stem}.value-contract.json")
    endforeach()

    lux_add_codegen_job(
        NAME ${job}
        GENERATOR "${LUX_META_GENERATOR}"
        MARKER luxref
        SOURCE_FILE "${P_SOURCE_FILE}"
        TARGET_FILES ${P_HEADERS}
        LOGICAL_PATHS ${logical_paths}
    )

    lux_codegen_add_projection(
        JOB ${job}
        NAME value_contract
        TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../template/value_contract.template"
        OUTPUT_ROOT "${output_root}"
        OUTPUT_SUFFIX .value-contract.json
    )

    lux_target_add_codegen(
        TARGET ${P_SOURCE_TARGET}
        JOB ${job}
        DONT_ADD_TO_SOURCE
        DONT_INCLUDE
    )

    set(${P_OUTPUT_VARIABLE} ${outputs} PARENT_SCOPE)
endfunction()
