# get sublist
subdirectory_list(dir_list)

foreach(subdir ${dir_list})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt)
        add_subdirectory(${subdir})
    endif()
endforeach()

install(
	EXPORT		lux::engine::function
	DESTINATION ${CMAKE_INSTALL_PREFIX}/share/lux-engine
	NAMESPACE   ${PROJECT_NAMESPACE}::function::
	FILE		${function_config_targets_file}
)
