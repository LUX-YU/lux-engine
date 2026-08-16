include_guard(GLOBAL)

set(LUX_HOST_TOOLS_PREFIX "" CACHE PATH
    "Install prefix containing host executables used by a cross build")

function(lux_configure_host_tools)
    if(CMAKE_CROSSCOMPILING AND NOT LUX_HOST_TOOLS_PREFIX)
        message(FATAL_ERROR
            "Cross builds require LUX_HOST_TOOLS_PREFIX. It must name the "
            "host-native install prefix containing lux_meta_generator, "
            "lux_asset_packer and lux_shader_emitter."
        )
    endif()

    if(CMAKE_CROSSCOMPILING AND NOT LUX_HOST_META_DIR)
        set(LUX_HOST_META_DIR
            "${LUX_HOST_TOOLS_PREFIX}/share/lux-engine/meta_gen"
            CACHE PATH
            "Host-generated reflection sources consumed by cross builds"
            FORCE
        )
    endif()
endfunction()

function(lux_find_host_program output)
    cmake_parse_arguments(ARG "REQUIRED" "" "NAMES" ${ARGN})
    if(NOT ARG_NAMES)
        message(FATAL_ERROR "lux_find_host_program requires NAMES")
    endif()

    if(CMAKE_CROSSCOMPILING)
        find_program(found_program
            NAMES ${ARG_NAMES}
            PATHS
                "${LUX_HOST_TOOLS_PREFIX}/bin"
                "${LUX_HOST_TOOLS_PREFIX}/tools"
                "${LUX_HOST_TOOLS_PREFIX}"
            NO_DEFAULT_PATH
            NO_CACHE
        )
    else()
        find_program(found_program NAMES ${ARG_NAMES} NO_CACHE)
    endif()

    if(ARG_REQUIRED AND NOT found_program)
        string(JOIN ", " names ${ARG_NAMES})
        message(FATAL_ERROR
            "Could not find host program (${names}). Host-tools prefix: "
            "'${LUX_HOST_TOOLS_PREFIX}'."
        )
    endif()
    set(${output} "${found_program}" PARENT_SCOPE)
endfunction()
