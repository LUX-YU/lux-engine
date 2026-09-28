include_guard(GLOBAL)
include(CMakeDependentOption)
set(LUX_EDITOR_MIGRATION_STAGE P00 CACHE STRING "Current V4 Editor migration gate (P00..P13)")

# Test requirements, not product feature switches. Multiple requirements are conjunctive.
foreach(group NATIVE DESKTOP GPU TOOLCHAIN INSTALLED)
    if(group STREQUAL "NATIVE")
        set(default ON)
    else()
        set(default OFF)
    endif()
    cmake_dependent_option(LUX_EDITOR_BUILD_${group}_TESTS
        "Build Editor ${group} tests" ${default} "BUILD_TESTING" OFF)
endforeach()

set(LUX_EDITOR_TEST_DESKTOP_GPU OFF)
if(LUX_EDITOR_BUILD_DESKTOP_TESTS AND LUX_EDITOR_BUILD_GPU_TESTS)
    set(LUX_EDITOR_TEST_DESKTOP_GPU ON)
endif()
set(LUX_EDITOR_TEST_INSTALLED_UI OFF)
if(LUX_EDITOR_TEST_DESKTOP_GPU AND LUX_EDITOR_BUILD_INSTALLED_TESTS)
    set(LUX_EDITOR_TEST_INSTALLED_UI ON)
endif()

# These helpers are deliberately limited to the current migration's test targets.
function(lux_editor_test_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endfunction()
