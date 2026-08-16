if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "" OR
   NOT DEFINED BINARY OR BINARY STREQUAL "")
    message(FATAL_ERROR
        "WriteRuntimeDependencyInventory requires OUTPUT and BINARY")
endif()

if(NOT DEFINED SUBJECT OR SUBJECT STREQUAL "")
    set(SUBJECT "runtime binary")
endif()

cmake_path(GET BINARY PARENT_PATH _binary_dir)
set(_dependency_arguments
    DIRECTORIES "${_binary_dir}"
    RESOLVED_DEPENDENCIES_VAR _dependencies
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    PRE_EXCLUDE_REGEXES
        "api-ms-.*"
        "ext-ms-.*"
    POST_EXCLUDE_REGEXES
        ".*[\\/]Windows[\\/]System32[\\/].*"
        ".*[\\/]WINDOWS[\\/]system32[\\/].*"
)
if(DEFINED BINARY_KIND AND BINARY_KIND STREQUAL "LIBRARY")
    file(GET_RUNTIME_DEPENDENCIES
        LIBRARIES "${BINARY}"
        ${_dependency_arguments}
    )
else()
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "${BINARY}"
        ${_dependency_arguments}
    )
endif()

if(_unresolved)
    list(JOIN _unresolved "\n  " _unresolved_text)
    message(FATAL_ERROR
        "Unresolved ${SUBJECT} dependencies:\n  ${_unresolved_text}")
endif()

set(_names)
foreach(_dependency IN LISTS _dependencies)
    cmake_path(GET _dependency FILENAME _name)
    list(APPEND _names "${_name}")
endforeach()

list(REMOVE_DUPLICATES _names)
list(SORT _names)

set(_contents "# Generated from ${SUBJECT}'s resolved PE/ELF runtime closure.\n")
foreach(_name IN LISTS _names)
    string(APPEND _contents "${_name}\n")
endforeach()
file(WRITE "${OUTPUT}" "${_contents}")
