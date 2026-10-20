include_guard(GLOBAL)

find_package(Vulkan 1.3 REQUIRED)

foreach(tool IN ITEMS spirv-cross-c-shared SPIRV-Tools-shared)
    string(REPLACE "-" "_" name "${tool}")
    if(NOT TARGET lux_render_${name})
        find_library(LUX_RENDER_${name}_LIBRARY NAMES ${tool}
            HINTS "${Vulkan_INCLUDE_DIR}/../Lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED
        )
        get_filename_component(tool_library_directory "${LUX_RENDER_${name}_LIBRARY}" DIRECTORY)
        if(tool STREQUAL "spirv-cross-c-shared")
            set(tool_header "spirv_cross/spirv_cross_c.h")
        else()
            set(tool_header "spirv-tools/libspirv.h")
        endif()
        # Shader compiler headers belong to the selected compiler library, not the Vulkan
        # loader/header package. A vcpkg Vulkan package need not contain either compiler.
        find_path(LUX_RENDER_${name}_INCLUDE NAMES "${tool_header}"
            HINTS "${tool_library_directory}/../Include" "$ENV{VULKAN_SDK}/Include"
            NO_DEFAULT_PATH REQUIRED
        )
        add_library(lux_render_${name} UNKNOWN IMPORTED GLOBAL)
        set_target_properties(lux_render_${name} PROPERTIES
            IMPORTED_LOCATION "${LUX_RENDER_${name}_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LUX_RENDER_${name}_INCLUDE}"
        )
        if(WIN32)
            find_file(LUX_RENDER_${name}_RUNTIME NAMES "${tool}.dll"
                HINTS "${tool_library_directory}/../Bin" "$ENV{VULKAN_SDK}/Bin"
                NO_DEFAULT_PATH REQUIRED
            )
            install(FILES "${LUX_RENDER_${name}_RUNTIME}" DESTINATION bin)
        endif()
    endif()
endforeach()
