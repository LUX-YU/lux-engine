message("-------- Plaform Layer --------")
# get sublist
subdirectory_list(dir_list)

if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    # Offline tools do not configure interactive platform domains.
    set(dir_list)
    if(LUX_ENABLE_FLOWFORGE_MLIR AND ENABLE_FLOWFORGE_TEST)
        list(APPEND dir_list dynamic_library)
    endif()
endif()

foreach(subdir ${dir_list})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt)
        add_subdirectory(${subdir})
    endif()
endforeach()

# filewatch 只在非 Android 构建(见其 CMakeLists 头注释)——安装列表跟着条件。
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(LUX_PLATFORM_COMPONENTS)
    if(LUX_ENABLE_FLOWFORGE_MLIR AND ENABLE_FLOWFORGE_TEST)
        list(APPEND LUX_PLATFORM_COMPONENTS dynamic_library)
    endif()
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
