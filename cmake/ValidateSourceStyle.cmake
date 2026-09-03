cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED LUX_SOURCE_DIR)
    message(FATAL_ERROR "LUX_SOURCE_DIR is required")
endif()

file(TO_CMAKE_PATH "${LUX_SOURCE_DIR}" source_root)
file(GLOB_RECURSE engine_sources LIST_DIRECTORIES false
    "${source_root}/engine/*.c"
    "${source_root}/engine/*.cpp"
    "${source_root}/engine/*.h"
    "${source_root}/engine/*.hpp"
    "${source_root}/engine/*.inl"
    "${source_root}/engine/*.ipp"
)
list(FILTER engine_sources EXCLUDE REGEX "/third_party/")

set(violations)
foreach(source IN LISTS engine_sources)
    file(READ "${source}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(REPLACE "\r" "\n" contents "${contents}")
    set(line_number 0)
    while(NOT contents STREQUAL "")
        string(FIND "${contents}" "\n" newline_position)
        if(newline_position EQUAL -1)
            set(line "${contents}")
            set(contents "")
        else()
            string(SUBSTRING "${contents}" 0 ${newline_position} line)
            math(EXPR next_line_position "${newline_position} + 1")
            string(SUBSTRING "${contents}" ${next_line_position} -1 contents)
        endif()
        math(EXPR line_number "${line_number} + 1")
        string(LENGTH "${line}" line_length)
        if(line_length GREATER 120)
            list(APPEND violations "${source}:${line_number}: line exceeds 120 columns (${line_length})")
        endif()

        string(FIND "${line}" "\t" tab_position)
        if(NOT tab_position EQUAL -1)
            list(APPEND violations "${source}:${line_number}: tab indentation is forbidden")
        endif()

        if(line MATCHES "[ \t]+$")
            list(APPEND violations "${source}:${line_number}: trailing whitespace")
        endif()

        if(line MATCHES "^[ \t]*(if|for|while|switch|catch)[ \t]*\\(.*\\)[ \t]*\\{")
            list(APPEND violations "${source}:${line_number}: control-statement brace must start the next line")
        endif()
        if(line MATCHES "^[ \t]*(else|do)[ \t]*\\{")
            list(APPEND violations "${source}:${line_number}: control-statement brace must start the next line")
        endif()
        if(line MATCHES "^[ \t]*(class|struct|enum class|enum struct)[^;]*\\{[ \t]*$")
            list(APPEND violations "${source}:${line_number}: type brace must start the next line")
        endif()

        if(line MATCHES "^[ \t]*enum[ \t]+" AND
           NOT line MATCHES "^[ \t]*enum[ \t]+(class|struct)[ \t]+")
            list(APPEND violations "${source}:${line_number}: use enum class")
        endif()

        string(FIND "${line}" "[" square_bracket)
        if(square_bracket EQUAL -1 AND
           line MATCHES
               "^[ \t]*([A-Za-z_~][A-Za-z0-9_:<>,*&]*[ \t]+)+[A-Za-z_~][A-Za-z0-9_:~]*[ \t]*\\([^;{}]*\\)[ \t]*(const[ \t]*)?(noexcept[ \t]*)?\\{[ \t]*$")
            list(APPEND violations "${source}:${line_number}: function brace must start the next line")
        endif()
    endwhile()
endforeach()

list(LENGTH violations violation_count)
if(violation_count GREATER 0)
    list(JOIN violations "\n  " violation_report)
    message(FATAL_ERROR "Engine source style violations (${violation_count}):\n  ${violation_report}")
endif()

message(STATUS "Engine source style is clean (${source_root}/engine)")
