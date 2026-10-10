include_guard(GLOBAL)

# Use the existing luxop parser and multi-projection generator. Only the Render
# projection changes; no second parser or protocol generator is introduced.
find_file(
    LUX_TRANSPORT_META_CMAKE meta.cmake
    HINTS "${lux-cxx_DIR}/reflection_generator/cmake_scripts"
    NO_DEFAULT_PATH REQUIRED
)

find_program(
    LUX_META_GENERATOR_EXECUTABLE lux_meta_generator
    HINTS "${lux-cxx_DIR}/../../bin"
    NO_DEFAULT_PATH REQUIRED
)

include("${LUX_TRANSPORT_META_CMAKE}")

function(render_transport_operations target author_header)
    get_filename_component(stem "${author_header}" NAME_WE)
    set(job "${target}_operations")
    lux_add_codegen_job(
        NAME "${job}"
        MARKER luxop
        TARGET_FILES "${author_header}"
        LOGICAL_PATHS "${stem}.hpp"
        EXTRA_COMPILE_OPTIONS -D__LUX_PARSE_TIME__=1
    )

    lux_codegen_add_projection(
        JOB "${job}"
        NAME transport_traits
        TEMPLATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/transport_ops.template"
        OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated"
        OUTPUT_SUFFIX .ops.hpp
    )

    lux_target_add_codegen(TARGET "${target}" JOB "${job}")
endfunction()
