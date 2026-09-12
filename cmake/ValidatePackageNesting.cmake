# ER-1 deliberately places two independently configured packages below semantic
# namespaces which also contain a package. The Editor collection owns their
# add_subdirectory calls; neither enclosing package may aggregate them.
function(lux_validate_package_nesting source_root package_root child)
    file(RELATIVE_PATH relative_child "${source_root}" "${child}")
    set(editor_nested_packages
        engine/editor/application/tooling
        engine/editor/ui/scene
    )
    if(relative_child IN_LIST editor_nested_packages)
        file(READ "${package_root}/CMakeLists.txt" parent_cmake)
        file(READ "${source_root}/engine/editor/CMakeLists.txt" collection_cmake)
        get_filename_component(child_name "${child}" NAME)
        string(REGEX REPLACE "^engine/editor/" "" collection_child "${relative_child}")
        if(parent_cmake MATCHES "add_subdirectory[ \t\r\n]*[(][ \t\r\n]*[\"]?${child_name}([\" \t\r\n)]|/)")
            message(FATAL_ERROR "Architecture: enclosing package may not configure '${relative_child}'.")
        endif()
        if(NOT collection_cmake MATCHES
            "add_subdirectory[ \t\r\n]*[(][ \t\r\n]*[\"]?${collection_child}[\" \t\r\n)]")
            message(FATAL_ERROR "Architecture: Editor collection must configure '${relative_child}'.")
        endif()
        return()
    endif()
    message(FATAL_ERROR "Architecture: package '${package_root}' also aggregates child package '${child}'.")
endfunction()
