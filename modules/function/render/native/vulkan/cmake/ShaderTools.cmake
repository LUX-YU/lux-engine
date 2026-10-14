include_guard(GLOBAL)

find_package(Vulkan 1.3 REQUIRED)

foreach(tool IN ITEMS spirv-cross-c-shared SPIRV-Tools-shared)
    string(REPLACE "-" "_" name "${tool}")
    if(NOT TARGET lux_render_${name})
        find_library(LUX_RENDER_${name}_LIBRARY NAMES ${tool}
            HINTS "${Vulkan_INCLUDE_DIR}/../Lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED
        )
        add_library(lux_render_${name} UNKNOWN IMPORTED GLOBAL)
        set_target_properties(lux_render_${name} PROPERTIES
            IMPORTED_LOCATION "${LUX_RENDER_${name}_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}"
        )
    endif()
endforeach()
