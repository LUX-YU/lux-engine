# embed_builtin_shaders.cmake
# Generates C++ headers with constexpr byte arrays from .luxasset shader files.
#
# Usage:
#   embed_builtin_shaders(
#       TARGET          <cmake_target>          # C++ target to attach generated sources to
#       ASSET_DIR       <dir>                   # directory containing .luxasset files
#       OUTPUT_DIR      <dir>                   # where to write generated .hpp headers
#       NAMESPACE       <ns>                    # C++ namespace for all generated arrays
#       SHADERS         <name1> <name2> ...     # bare shader names (e.g. grid/grid.vert)
#   )
#
# For each shader "grid/grid.vert":
#   Input:  ${ASSET_DIR}/shaders/grid/grid.vert.luxasset
#   Output: ${OUTPUT_DIR}/grid_vert_embed.hpp
#   Arrays: grid_vert_spirv[], grid_vert_info[] in namespace ${NAMESPACE}

if(_EMBED_BUILTIN_SHADERS_INCLUDED_)
    return()
endif()
set(_EMBED_BUILTIN_SHADERS_INCLUDED_ TRUE)

function(embed_builtin_shaders)
    set(_one_value_args TARGET ASSET_DIR OUTPUT_DIR NAMESPACE)
    set(_multi_value_args SHADERS)
    cmake_parse_arguments(ARGS "" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    if(NOT ARGS_TARGET)
        message(FATAL_ERROR "[embed_builtin_shaders] TARGET is required")
    endif()
    if(NOT ARGS_ASSET_DIR)
        message(FATAL_ERROR "[embed_builtin_shaders] ASSET_DIR is required")
    endif()
    if(NOT ARGS_OUTPUT_DIR)
        set(ARGS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/builtin_shaders")
    endif()
    if(NOT ARGS_NAMESPACE)
        set(ARGS_NAMESPACE "lux::render::builtin")
    endif()

    # Locate packer
    if(LUX_ASSET_PACKER_EXECUTABLE)
        if(NOT EXISTS "${LUX_ASSET_PACKER_EXECUTABLE}")
            message(FATAL_ERROR
                "[embed_builtin_shaders] LUX_ASSET_PACKER_EXECUTABLE does not exist: "
                "${LUX_ASSET_PACKER_EXECUTABLE}"
            )
        endif()
        set(_packer "${LUX_ASSET_PACKER_EXECUTABLE}")
        set(_packer_dep "")
    elseif(TARGET lux_asset_packer)
        set(_packer "$<TARGET_FILE:lux_asset_packer>")
        set(_packer_dep lux_asset_packer)
    else()
        if(COMMAND lux_find_host_program)
            lux_find_host_program(
                _packer
                REQUIRED
                NAMES lux_asset_packer
            )
        elseif(CMAKE_CROSSCOMPILING)
            message(FATAL_ERROR
                "[embed_builtin_shaders] Cross builds require lux_find_host_program"
            )
        else()
            find_program(_packer lux_asset_packer REQUIRED)
        endif()
        set(_packer_dep "")
    endif()

    file(MAKE_DIRECTORY "${ARGS_OUTPUT_DIR}")

    set(_all_outputs "")
    set(_all_embed_names "")

    foreach(_shader ${ARGS_SHADERS})
        # Convert "grid/grid.vert" -> "grid_vert" (embed name)
        string(REPLACE "/" "_" _embed_name "${_shader}")
        string(REPLACE "." "_" _embed_name "${_embed_name}")

        set(_luxasset "${ARGS_ASSET_DIR}/shaders/${_shader}.luxasset")
        set(_output   "${ARGS_OUTPUT_DIR}/${_embed_name}_embed.hpp")

        add_custom_command(
            OUTPUT "${_output}"
            COMMAND ${_packer}
                --embed
                --target_path "${_luxasset}"
                --source_path "${_output}"
                --embed_name  "${_embed_name}"
                --embed_namespace "${ARGS_NAMESPACE}"
            DEPENDS "${_luxasset}" ${_packer_dep}
            COMMENT "[embed_builtin_shaders] ${_shader} -> ${_embed_name}_embed.hpp"
            VERBATIM
        )

        list(APPEND _all_outputs "${_output}")
        list(APPEND _all_embed_names "${_embed_name}")
    endforeach()

    # Custom target to drive generation
    set(_gen_target "${ARGS_TARGET}_builtin_embed")
    add_custom_target(${_gen_target}
        DEPENDS ${_all_outputs}
        COMMENT "[embed_builtin_shaders] Generating builtin shader headers"
    )

    # Ensure the C++ target depends on generation and can find the headers
    add_dependencies(${ARGS_TARGET} ${_gen_target})
    target_include_directories(${ARGS_TARGET} PRIVATE "${ARGS_OUTPUT_DIR}")
    # Store generated output dir as a target property for downstream use
    set_target_properties(${ARGS_TARGET} PROPERTIES
        BUILTIN_SHADER_EMBED_DIR "${ARGS_OUTPUT_DIR}"
    )
endfunction()
