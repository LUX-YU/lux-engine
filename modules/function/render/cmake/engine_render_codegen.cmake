# Render-owned code generation projections.
# Parsing is provided by lux-cxx; these templates and functions belong to Render.

include_guard(GLOBAL)

# =============================================================================
# engine_add_pass_params
#   Renders the two PassParams artifacts from ONE annotated header (marker
#   family `luxpass` — see MetaAnnotations.hpp):
#     <Stem>.pass.hpp  C++ side: PC layout constants + offset static_asserts,
#                      declareGraphIO / createTransientDS / pushScalars
#     <Stem>.lglslh    GLSL side: local uniforms (declaration order) + PC block
#   Both land in ${CMAKE_CURRENT_BINARY_DIR}/pass_gen, which is added to
#   TARGET's private include path. The .lglslh must additionally be routed
#   through emit_lglsl_shaders (a second call with SOURCE_DIR = pass_gen) so
#   the shader emitter injects set/binding before glslc sees it.
#
#   The luxpass marker is disjoint from luxref on purpose: the params struct
#   (holding RG/Vulkan handles) stays invisible to editor reflection; the
#   scalars struct carries BOTH markers when it also feeds the inspector.
#
#   Parameters:
#     NAME                  – unique base name for the two meta targets
#     TARGET                – consumer target (dependency + include path)
#     TARGET_FILES          – annotated header(s); one params struct per header
#     EXTRA_COMPILE_OPTIONS – extra parse flags (appended after parse-time flag)
#     OUT_DIR_VAR           – [out, optional] variable receiving the pass_gen dir
# =============================================================================
function(engine_add_pass_params)
    set(one_value_args NAME TARGET OUT_DIR_VAR)
    set(multi_value_args TARGET_FILES EXTRA_COMPILE_OPTIONS)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARGS_NAME)
        message(FATAL_ERROR "[engine_add_pass_params] NAME is required")
    endif()
    if(NOT ARGS_TARGET)
        message(FATAL_ERROR "[engine_add_pass_params] TARGET is required")
    endif()
    if(NOT ARGS_TARGET_FILES)
        message(FATAL_ERROR "[engine_add_pass_params] TARGET_FILES is required")
    endif()

    set(_hpp_template   "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template/pass_params_hpp.template")
    set(_glslh_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template/pass_params_glslh.template")
    set(_out_dir        "${CMAKE_CURRENT_BINARY_DIR}/pass_gen")

    set(_logical_paths)
    foreach(_header IN LISTS ARGS_TARGET_FILES)
        get_filename_component(_name "${_header}" NAME)
        list(APPEND _logical_paths "${_name}")
    endforeach()
    lux_add_codegen_job(
        NAME ${ARGS_NAME}_pass_codegen
        MARKER luxpass
        TARGET_FILES ${ARGS_TARGET_FILES}
        LOGICAL_PATHS ${_logical_paths}
        PARSE_INCLUDED_MARKED
        EXTRA_COMPILE_OPTIONS
            -D__LUX_PARSE_TIME__=1
            ${ARGS_EXTRA_COMPILE_OPTIONS}
    )
    lux_codegen_add_projection(
        JOB ${ARGS_NAME}_pass_codegen
        NAME pass_hpp
        TEMPLATE ${_hpp_template}
        OUTPUT_ROOT ${_out_dir}
        OUTPUT_SUFFIX .pass.hpp
    )
    lux_codegen_add_projection(
        JOB ${ARGS_NAME}_pass_codegen
        NAME pass_glslh
        TEMPLATE ${_glslh_template}
        OUTPUT_ROOT ${_out_dir}
        OUTPUT_SUFFIX .lglslh
    )
    lux_target_add_codegen(
        TARGET ${ARGS_TARGET}
        JOB ${ARGS_NAME}_pass_codegen
    )

    if(ARGS_OUT_DIR_VAR)
        set(${ARGS_OUT_DIR_VAR} ${_out_dir} PARENT_SCOPE)
    endif()
    message(STATUS "[pass-params] ${ARGS_TARGET} <- ${ARGS_NAME} (${_out_dir})")
endfunction()
# =============================================================================
# engine_add_comm_ops
#   Generates a feature's whole comm/Operation face from ONE annotated header
#   (marker family `luxop` — see MetaAnnotations.hpp):
#     <Stem>.ops.hpp  op descriptors + CommandTraits + OpIds + payload-taking
#                     Proxy declaration (installed client surface)
#     <Stem>.client.ops.cpp  proxy/client bodies (compiled into CLIENT_TARGET)
#     <Stem>.ops.cpp         createFn/registrar/factory (compiled into
#                            IMPLEMENTATION_TARGET; declares the hand-written
#                            handle<OpName> contracts)
#   Outputs land under comm_gen/<INCLUDE_PREFIX>/ so the generated public include
#   spelling is identical
#   in-tree (BUILD_INTERFACE root) and installed (FILES_MATCHING *.ops.hpp).
#
#   Parameters:
#     NAME / INCLUDE_PREFIX / TARGET_FILES / EXTRA_COMPILE_OPTIONS — as usual.
#     CLIENT_TARGET and IMPLEMENTATION_TARGET may be supplied together or in
#     separate calls.  This lets the backend-neutral client own its generated
#     public surface even when a build profile intentionally omits the backend
#     implementation target.  An implementation-only call must already obtain
#     the generated public include directory through its client dependency.
#     One add_meta output is created per requested face and header (the cpp
#     templates need a per-file `stem`).
# =============================================================================
function(engine_add_comm_ops)
    set(one_value_args NAME CLIENT_TARGET IMPLEMENTATION_TARGET INCLUDE_PREFIX)
    set(multi_value_args TARGET_FILES EXTRA_COMPILE_OPTIONS)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARGS_NAME OR NOT ARGS_INCLUDE_PREFIX OR NOT ARGS_TARGET_FILES OR
       (NOT ARGS_CLIENT_TARGET AND NOT ARGS_IMPLEMENTATION_TARGET))
        message(FATAL_ERROR
            "[engine_add_comm_ops] NAME, INCLUDE_PREFIX, TARGET_FILES and at "
            "least one of CLIENT_TARGET / IMPLEMENTATION_TARGET are required"
        )
    endif()

    if(ARGS_CLIENT_TARGET)
        if(NOT TARGET ${ARGS_CLIENT_TARGET})
            message(FATAL_ERROR
                "[engine_add_comm_ops] CLIENT_TARGET "
                "'${ARGS_CLIENT_TARGET}' does not exist"
            )
        endif()
    endif()
    if(ARGS_IMPLEMENTATION_TARGET)
        if(NOT TARGET ${ARGS_IMPLEMENTATION_TARGET})
            message(FATAL_ERROR
                "[engine_add_comm_ops] IMPLEMENTATION_TARGET "
                "'${ARGS_IMPLEMENTATION_TARGET}' does not exist"
            )
        endif()
    endif()

    if(IS_ABSOLUTE "${ARGS_INCLUDE_PREFIX}" OR ARGS_INCLUDE_PREFIX MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR
            "[engine_add_comm_ops] INCLUDE_PREFIX must be an install-relative path"
        )
    endif()

    set(_hpp_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template/comm_ops_hpp.template")
    set(_client_cpp_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template/comm_ops_client_cpp.template")
    set(_cpp_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template/comm_ops_cpp.template")
    set(_root    "${CMAKE_CURRENT_BINARY_DIR}/comm_gen")
    set(_out_dir "${_root}/${ARGS_INCLUDE_PREFIX}")
    file(MAKE_DIRECTORY "${_out_dir}")

    foreach(_header IN LISTS ARGS_TARGET_FILES)
        get_filename_component(_stem "${_header}" NAME_WE)
        set(_job ${ARGS_NAME}_${_stem}_comm_codegen)
        set(_logical "${ARGS_INCLUDE_PREFIX}/${_stem}.hpp")
        if(TARGET ${_job})
            set(_existing_job TRUE)
            get_target_property(
                _job_root ${_job} LUX_CODEGEN_ops_cpp_OUTPUT_ROOT
            )
        else()
            set(_existing_job FALSE)
            set(_job_root "${_root}")
            lux_add_codegen_job(
                NAME ${_job}
                MARKER luxop
                TARGET_FILES ${_header}
                LOGICAL_PATHS ${_logical}
                EXTRA_COMPILE_OPTIONS
                    -D__LUX_PARSE_TIME__=1
                    ${ARGS_EXTRA_COMPILE_OPTIONS}
            )
        endif()
        if(ARGS_CLIENT_TARGET AND NOT _existing_job)
            lux_codegen_add_projection(
                JOB ${_job}
                NAME ops_hpp
                TEMPLATE ${_hpp_template}
                OUTPUT_ROOT ${_job_root}
                OUTPUT_SUFFIX .ops.hpp
                JSON_FIELD "{\"stem\":\"${_stem}\"}"
            )
            lux_codegen_add_projection(
                JOB ${_job}
                NAME client_ops_cpp
                TEMPLATE ${_client_cpp_template}
                OUTPUT_ROOT ${_job_root}
                OUTPUT_SUFFIX .client.ops.cpp
                JSON_FIELD "{\"stem\":\"${_stem}\"}"
            )
            lux_codegen_add_projection(
                JOB ${_job}
                NAME ops_cpp
                TEMPLATE ${_cpp_template}
                OUTPUT_ROOT ${_job_root}
                OUTPUT_SUFFIX .ops.cpp
                JSON_FIELD "{\"stem\":\"${_stem}\",\"include_prefix\":\"${ARGS_INCLUDE_PREFIX}\"}"
            )
        endif()

        if(ARGS_IMPLEMENTATION_TARGET AND NOT _existing_job AND
           NOT ARGS_CLIENT_TARGET)
            lux_codegen_add_projection(
                JOB ${_job}
                NAME ops_cpp
                TEMPLATE ${_cpp_template}
                OUTPUT_ROOT ${_job_root}
                OUTPUT_SUFFIX .ops.cpp
                JSON_FIELD "{\"stem\":\"${_stem}\",\"include_prefix\":\"${ARGS_INCLUDE_PREFIX}\"}"
            )
        endif()

        if(NOT _existing_job)
            if(ARGS_CLIENT_TARGET)
                set(_generation_owner ${ARGS_CLIENT_TARGET})
            else()
                set(_generation_owner ${ARGS_IMPLEMENTATION_TARGET})
            endif()
            lux_target_add_codegen(
                TARGET ${_generation_owner}
                JOB ${_job}
                DONT_ADD_TO_SOURCE
                DONT_INCLUDE
            )
        endif()
        if(ARGS_CLIENT_TARGET)
            target_sources(
                ${ARGS_CLIENT_TARGET}
                PRIVATE
                    "${_job_root}/${ARGS_INCLUDE_PREFIX}/${_stem}.ops.hpp"
                    "${_job_root}/${ARGS_INCLUDE_PREFIX}/${_stem}.client.ops.cpp"
            )
            set_source_files_properties(
                "${_job_root}/${ARGS_INCLUDE_PREFIX}/${_stem}.ops.hpp"
                "${_job_root}/${ARGS_INCLUDE_PREFIX}/${_stem}.client.ops.cpp"
                PROPERTIES GENERATED TRUE
            )
        endif()
        if(ARGS_IMPLEMENTATION_TARGET)
            target_sources(
                ${ARGS_IMPLEMENTATION_TARGET}
                PRIVATE "${_job_root}/${ARGS_INCLUDE_PREFIX}/${_stem}.ops.cpp"
            )
            set_source_files_properties(
                "${_job_root}/${ARGS_INCLUDE_PREFIX}/${_stem}.ops.cpp"
                PROPERTIES GENERATED TRUE
            )
            if(_existing_job OR
               NOT "${ARGS_IMPLEMENTATION_TARGET}" STREQUAL "${_generation_owner}")
                add_dependencies(
                    ${ARGS_IMPLEMENTATION_TARGET}
                    ${_job}_generate
                )
            endif()
        endif()
    endforeach()

    if(ARGS_CLIENT_TARGET)
        target_include_directories(
            ${ARGS_CLIENT_TARGET}
            PUBLIC "$<BUILD_INTERFACE:${_root}>"
        )
        install(DIRECTORY "${_root}/" DESTINATION include
                FILES_MATCHING PATTERN "*.ops.hpp")
        message(STATUS
            "[comm-ops] ${ARGS_CLIENT_TARGET}:client <- ${ARGS_NAME} (${_out_dir})"
        )
    endif()

    if(ARGS_IMPLEMENTATION_TARGET)
        target_include_directories(
            ${ARGS_IMPLEMENTATION_TARGET}
            PRIVATE "$<BUILD_INTERFACE:${_root}>"
        )
        message(STATUS
            "[comm-ops] ${ARGS_IMPLEMENTATION_TARGET}:implementation "
            "<- ${ARGS_NAME} (${_out_dir})"
        )
    endif()
endfunction()
