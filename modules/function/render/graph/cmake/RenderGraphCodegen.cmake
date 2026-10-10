include_guard(GLOBAL)

find_file(
    LUX_GRAPH_META_CMAKE meta.cmake
    HINTS "${lux-cxx_DIR}/reflection_generator/cmake_scripts"
    NO_DEFAULT_PATH REQUIRED
)

include("${LUX_GRAPH_META_CMAKE}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(render_pass_schema target author_header)
    get_filename_component(stem "${author_header}" NAME_WE)
    set(job "${target}_schema")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/generated")
    lux_add_codegen_job(
        NAME "${job}"
        MARKER luxpass
        TARGET_FILES "${author_header}"
        LOGICAL_PATHS "${stem}.hpp"
        PARSE_INCLUDED_MARKED
        EXTRA_COMPILE_OPTIONS -D__LUX_PARSE_TIME__=1
    )

    lux_codegen_add_projection(
        JOB "${job}"
        NAME pass_schema
        TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/pass_schema.template"
        OUTPUT_ROOT "${output}"
        OUTPUT_SUFFIX .schema.json
    )

    lux_target_add_codegen(TARGET "${target}" JOB "${job}" DONT_ADD_TO_SOURCE)

    add_custom_command(
        OUTPUT "${output}/${stem}.pass.hpp" "${output}/${stem}.lglslh"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PassSchema.py"
            "${output}/${stem}.schema.json" "${author_header}" "${output}/${stem}"
        DEPENDS "${output}/${stem}.schema.json" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PassSchema.py"
        VERBATIM
    )

    add_custom_target("${target}_pass_schema" DEPENDS "${output}/${stem}.pass.hpp" "${output}/${stem}.lglslh")

    add_dependencies("${target}" "${target}_pass_schema")

    target_include_directories("${target}" PRIVATE "${output}")
endfunction()
