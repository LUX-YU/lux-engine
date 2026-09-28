# Use with CMAKE_PROJECT_INCLUDE in a separate native-only build directory.
# This intercepts the actual test-tool discovery calls, without breaking MSVC's own linker.
function(find_program)
    if(ARGV0 MATCHES "^EDITOR_TEST_(LINKER|POWERSHELL)$")
        message(FATAL_ERROR "Native-only configure attempted platform test-tool discovery: ${ARGV}")
    endif()
    _find_program(${ARGV})
    set(${ARGV0} "${${ARGV0}}" PARENT_SCOPE)
endfunction()
