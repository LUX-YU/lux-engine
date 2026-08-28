message("-------- Plaform Layer --------")
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    # Offline tools do not configure interactive platform domains.
    add_subdirectory(dynamic_library)
else()
    add_subdirectory(gapi)
    add_subdirectory(window)
    add_subdirectory(dynamic_library)
    if(NOT ANDROID)
        add_subdirectory(filewatch)
    endif()
endif()

# filewatch 只在非 Android 构建(见其 CMakeLists 头注释)——安装列表跟着条件。
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(LUX_PLATFORM_COMPONENTS dynamic_library)
else()
    set(LUX_PLATFORM_COMPONENTS
        gapi
        window
        dynamic_library
    )
    if(NOT ANDROID)
        list(APPEND LUX_PLATFORM_COMPONENTS filewatch)
    endif()
endif()

if(LUX_PLATFORM_COMPONENTS)
    install_components(
        PROJECT_NAME lux-engine-platform
        VERSION      0.0.1
        NAMESPACE    lux::engine::platform
        COMPONENTS   ${LUX_PLATFORM_COMPONENTS}
    )
endif()
