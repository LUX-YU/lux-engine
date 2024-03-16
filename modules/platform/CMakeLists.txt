message("-------- Plaform Layer --------")
# get sublist
subdirectory_list(dir_list)

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_PLATFORM_LIBRARY
    PUBLIC_MACRO_NAME   LUX_PLATFORM_PUBLIC
    GENERATE_FILE_PATH  lux/engine/platform/visibility.h
)

foreach(subdir ${dir_list})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt)
        add_subdirectory(${subdir})
    endif()
endforeach()

install_components(
	PROJECT_NAME	lux-engine-platform
	VERSION			0.0.1
	NAMESPACE		lux::engine::platform
	COMPONENTS		RHI
                    event
					window
)
