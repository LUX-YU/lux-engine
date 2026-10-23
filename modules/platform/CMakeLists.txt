message("-------- Plaform Layer --------")
add_subdirectory(process)
add_subdirectory(ipc)
if(LUX_BUILD_RENDER OR LUX_BUILD_WINDOW)
    add_subdirectory(gapi)
endif()

if(LUX_BUILD_WINDOW)
    add_subdirectory(window)
endif()

add_subdirectory(dynamic_library)

set(LUX_PLATFORM_COMPONENTS dynamic_library)

if(TARGET gapi)
    list(APPEND LUX_PLATFORM_COMPONENTS gapi)
endif()

if(TARGET window)
    list(APPEND LUX_PLATFORM_COMPONENTS window)
endif()

if(NOT ANDROID AND NOT LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    add_subdirectory(filewatch)
    list(APPEND LUX_PLATFORM_COMPONENTS filewatch)
endif()

if(LUX_PLATFORM_COMPONENTS)
    list(APPEND LUX_PLATFORM_COMPONENTS platform_process platform_ipc)
    lux_engine_install_components(
        PROJECT_NAME lux-engine-platform
        VERSION      0.0.1
        NAMESPACE    lux::engine::platform
        COMPONENTS   ${LUX_PLATFORM_COMPONENTS}
    )
endif()
