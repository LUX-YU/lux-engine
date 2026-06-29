message("-------- Plaform Layer --------")
# get sublist
subdirectory_list(dir_list)

foreach(subdir ${dir_list})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt)
        add_subdirectory(${subdir})
    endif()
endforeach()

install_components(
	PROJECT_NAME	lux-engine-platform
	VERSION			0.0.1
	NAMESPACE		lux::engine::platform
	COMPONENTS		common
					gapi
					window
					dynamic_library
)
