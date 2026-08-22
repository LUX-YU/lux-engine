include_guard(GLOBAL)

# The layer roots and composite domains are composition entry points, not
# independently installable modules. Their child directories are therefore
# intentionally excluded from the leaf layout check below.
set(_LUX_MODULE_LAYOUT_AGGREGATES
    ""
    "core"
    "function"
    "function/navigation"
    "function/render"
    "function/script"
    "platform"
    "resource"
)

set(_LUX_MODULE_LAYOUT_ALLOWED_DIRS
    include
    sinclude
    pinclude
    src
    test
    cmake
    third_party
    samples
    assets
    template
    generated
    data
)

function(lux_validate_module_layout modules_root)
    file(GLOB_RECURSE _module_cmake_files
        RELATIVE "${modules_root}"
        "${modules_root}/*/CMakeLists.txt"
    )

    set(_violations)
    foreach(_cmake_file IN LISTS _module_cmake_files)
        get_filename_component(_module_dir "${_cmake_file}" DIRECTORY)
        if(_module_dir IN_LIST _LUX_MODULE_LAYOUT_AGGREGATES)
            continue()
        endif()
        # CMakeLists.txt files below approved auxiliary roots describe tests,
        # shader/assets, or helper code; they are not module roots themselves.
        if(_module_dir MATCHES
            "(^|/)(test|cmake|third_party|samples|assets|template|generated|data)(/|$)"
        )
            continue()
        endif()

        file(GLOB _children LIST_DIRECTORIES true
            "${modules_root}/${_module_dir}/*"
        )
        foreach(_child IN LISTS _children)
            if(IS_DIRECTORY "${_child}")
                get_filename_component(_child_name "${_child}" NAME)
                if(NOT _child_name IN_LIST _LUX_MODULE_LAYOUT_ALLOWED_DIRS)
                    list(APPEND _violations
                        "${_module_dir}/${_child_name}"
                    )
                endif()
            endif()
        endforeach()
    endforeach()

    if(_violations)
        list(REMOVE_DUPLICATES _violations)
        list(JOIN _violations "\n  " _formatted)
        message(FATAL_ERROR
            "Module leaf layout violation. Functional directories must be "
            "inside include/sinclude/pinclude/src; auxiliary directories "
            "must be explicitly allowed.\n  ${_formatted}"
        )
    endif()
endfunction()
