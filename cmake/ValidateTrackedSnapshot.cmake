# Qualification starts from Git content, never an ignored local source overlay.
cmake_minimum_required(VERSION 3.22)
if(NOT DEFINED LUX_SOURCE_DIR)
    message(FATAL_ERROR "LUX_SOURCE_DIR is required")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=all
    WORKING_DIRECTORY "${LUX_SOURCE_DIR}" OUTPUT_VARIABLE changes
    RESULT_VARIABLE status_result OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT status_result EQUAL 0 OR NOT changes STREQUAL "")
    message(FATAL_ERROR "Qualification requires a clean tracked commit: ${changes}")
endif()
file(STRINGS "${LUX_SOURCE_DIR}/.gitignore" ignore_lines)
foreach(line IN LISTS ignore_lines)
    if(line MATCHES "^[ \t]*test/?[ \t]*$")
        message(FATAL_ERROR "Bare test ignore rule hides source inputs")
    endif()
endforeach()
execute_process(COMMAND "${GIT_EXECUTABLE}" ls-files --deleted
    WORKING_DIRECTORY "${LUX_SOURCE_DIR}" OUTPUT_VARIABLE missing RESULT_VARIABLE files_result)
if(NOT files_result EQUAL 0 OR NOT missing STREQUAL "")
    message(FATAL_ERROR "Tracked source files are missing: ${missing}")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${LUX_SOURCE_DIR}" OUTPUT_VARIABLE commit
    RESULT_VARIABLE commit_result OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT commit_result EQUAL 0)
    message(FATAL_ERROR "Cannot resolve qualification commit")
endif()
message(STATUS "Clean tracked snapshot: ${commit}. Configure an independent clone of this commit next.")
