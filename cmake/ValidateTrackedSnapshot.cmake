cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED LUX_SOURCE_DIR)
    message(FATAL_ERROR "LUX_SOURCE_DIR is required")
endif()

find_package(Git REQUIRED)
file(REAL_PATH "${LUX_SOURCE_DIR}" source_root)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${source_root}" rev-parse --show-toplevel
    RESULT_VARIABLE root_result
    OUTPUT_VARIABLE git_root
    ERROR_VARIABLE root_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT root_result EQUAL 0)
    message(FATAL_ERROR "Tracked snapshot validation requires a Git worktree: ${root_error}")
endif()
file(REAL_PATH "${git_root}" normalized_git_root)
if(NOT normalized_git_root STREQUAL source_root)
    message(FATAL_ERROR
        "LUX_SOURCE_DIR must be the Git worktree root. Expected '${normalized_git_root}', got '${source_root}'."
    )
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${source_root}" status --porcelain=v1 --untracked-files=all
    RESULT_VARIABLE status_result
    OUTPUT_VARIABLE status_output
    ERROR_VARIABLE status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT status_result EQUAL 0)
    message(FATAL_ERROR "git status failed: ${status_error}")
endif()
if(NOT status_output STREQUAL "")
    message(FATAL_ERROR "Qualification requires a clean tracked snapshot:\n${status_output}")
endif()

foreach(diff_mode IN ITEMS unstaged staged)
    if(diff_mode STREQUAL "staged")
        set(diff_args diff --cached --exit-code)
    else()
        set(diff_args diff --exit-code)
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_root}" ${diff_args}
        RESULT_VARIABLE diff_result
        OUTPUT_QUIET
        ERROR_VARIABLE diff_error
    )
    if(NOT diff_result EQUAL 0)
        message(FATAL_ERROR "Qualification snapshot has ${diff_mode} changes: ${diff_error}")
    endif()
endforeach()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${source_root}" ls-files --others --ignored --exclude-standard
    RESULT_VARIABLE ignored_result
    OUTPUT_VARIABLE ignored_output
    ERROR_VARIABLE ignored_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT ignored_result EQUAL 0)
    message(FATAL_ERROR "git ls-files failed: ${ignored_error}")
endif()

set(hidden_sources)
if(NOT ignored_output STREQUAL "")
    string(REPLACE "\r\n" "\n" ignored_output "${ignored_output}")
    string(REPLACE "\n" ";" ignored_paths "${ignored_output}")
    foreach(path IN LISTS ignored_paths)
        if(path MATCHES "^(engine|modules|platforms|test|cmake)/" AND
           path MATCHES "(^|/)(CMakeLists\\.txt|[^/]+\\.(c|cc|cpp|cxx|h|hh|hpp|hxx|cmake))$")
            list(APPEND hidden_sources "${path}")
        endif()
    endforeach()
endif()
if(hidden_sources)
    list(JOIN hidden_sources "\n" hidden_report)
    message(FATAL_ERROR "Ignored source/build inputs are forbidden in qualification:\n${hidden_report}")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${source_root}" rev-parse HEAD
    OUTPUT_VARIABLE qualified_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
message(STATUS "Tracked snapshot is clean: ${qualified_revision}")
