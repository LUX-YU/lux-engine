cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED INSTALL_PREFIX OR INSTALL_PREFIX STREQUAL "")
    message(FATAL_ERROR "INSTALL_PREFIX is required")
endif()

file(TO_CMAKE_PATH "${INSTALL_PREFIX}" prefix)
if(NOT IS_DIRECTORY "${prefix}")
    message(FATAL_ERROR "Install prefix does not exist: ${prefix}")
endif()

file(GLOB_RECURSE installed_entries LIST_DIRECTORIES true "${prefix}/*")
foreach(entry IN LISTS installed_entries)
    file(TO_CMAKE_PATH "${entry}" normalized)
    if(normalized MATCHES "[/](legacy|sinclude|pinclude)([/]|$)" OR
       normalized MATCHES "[/]ecs[/](detail|core/detail|schedule/detail)([/]|$)")
        message(FATAL_ERROR
            "Install surface exposes quarantine/private detail: ${normalized}"
        )
    endif()
    get_filename_component(name "${entry}" NAME)
    if(name MATCHES
       "^(Registry|ISystem|SceneServices|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|AssetManager|AssetRef|AssetLoadPort|AssetServices)\\.(h|hpp)$")
        message(FATAL_ERROR "Install surface exposes retired API: ${normalized}")
    endif()
endforeach()

file(GLOB_RECURSE installed_text LIST_DIRECTORIES false
    "${prefix}/*.hpp"
    "${prefix}/*.h"
    "${prefix}/*.cmake"
)
foreach(entry IN LISTS installed_text)
    file(READ "${entry}" content)
    if(content MATCHES "[/\\\\]legacy[/\\\\]" OR
       content MATCHES "AssetManager|AssetRef|AssetLoadPort|AssetServices|SceneServices|ISystem|ScheduleBuilder|ScheduleMutationBatch|InstalledSystemBatch|cooked_relocation|LXES")
        message(FATAL_ERROR "Installed file contains a retired boundary: ${entry}")
    endif()
endforeach()

if(DEFINED INSTALL_MANIFEST AND EXISTS "${INSTALL_MANIFEST}")
    file(READ "${INSTALL_MANIFEST}" manifest)
    if(manifest MATCHES "[/\\\\]legacy[/\\\\]" OR
       manifest MATCHES "[/\\\\](sinclude|pinclude)[/\\\\]")
        message(FATAL_ERROR "Install manifest contains quarantine/private paths")
    endif()
endif()

message(STATUS "Installed architecture surface is clean: ${prefix}")
