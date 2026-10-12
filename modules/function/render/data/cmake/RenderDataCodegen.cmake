include_guard(GLOBAL)
include("${lux-cxx_DIR}/reflection_generator/cmake_scripts/meta.cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(render_data_schema target author_header)
    cmake_parse_arguments(DATA "SERIALIZATION" "OUTPUT_NAME" "" ${ARGN})
    if(DATA_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown DataSchema codegen option")
    endif()
    get_filename_component(stem "${author_header}" NAME_WE)
    if(DATA_OUTPUT_NAME)
        set(stem "${DATA_OUTPUT_NAME}")
    endif()
    if(IS_ABSOLUTE "${stem}" OR stem MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "DataSchema OUTPUT_NAME must stay within generated output")
    endif()
    string(SHA256 identity "${stem}")
    set(job "${target}_${identity}_data")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/generated")
    lux_add_codegen_job(
        NAME "${job}" MARKER luxdata
        TARGET_FILES "${author_header}" LOGICAL_PATHS "${stem}.hpp"
        PARSE_INCLUDED_MARKED EXTRA_COMPILE_OPTIONS -D__LUX_PARSE_TIME__=1
    )

    lux_codegen_add_projection(
        JOB "${job}" NAME data_schema
        TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/data_schema.template"
        OUTPUT_ROOT "${output}" OUTPUT_SUFFIX .schema.json
    )

    if(DATA_SERIALIZATION)
        find_package(lux-cxx CONFIG REQUIRED COMPONENTS serialization)
        include_component_cmake_scripts(lux::cxx::serialization)
        lux_codegen_add_projection(
            JOB "${job}" NAME serialization
            TEMPLATE "${LUX_SERIALIZATION_TEMPLATE}"
            OUTPUT_ROOT "${output}" OUTPUT_SUFFIX .serialize.hpp
        )
    endif()

    lux_target_add_codegen(TARGET "${target}" JOB "${job}" DONT_ADD_TO_SOURCE)

    if(NOT LUX_DATA_GPU_TOOLS)
        get_filename_component(LUX_DATA_GPU_TOOLS
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../../../engine/toolchain/shader/cmake" ABSOLUTE
        )
    endif()

    add_custom_command(
        OUTPUT "${output}/${stem}.data.hpp" "${output}/${stem}.lglslh"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DataSchema.py"
            "${output}/${stem}.schema.json" "${author_header}" "${output}/${stem}" "${LUX_DATA_GPU_TOOLS}"
        DEPENDS "${output}/${stem}.schema.json" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DataSchema.py"
            "${LUX_DATA_GPU_TOOLS}/GpuValues.py"
        VERBATIM
    )

    add_custom_target("${job}_projection" DEPENDS "${output}/${stem}.data.hpp" "${output}/${stem}.lglslh")

    if(NOT TARGET "${target}_data_schema")
        add_custom_target("${target}_data_schema")
    endif()

    add_dependencies("${target}_data_schema" "${job}_projection")

    add_dependencies("${target}" "${target}_data_schema")

    target_include_directories("${target}" PRIVATE "${output}")
endfunction()
