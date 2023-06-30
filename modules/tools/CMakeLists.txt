message("-------- Tools Layer --------")

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_TOOLS_LIBRARY
    PUBLIC_MACRO_NAME   LUX_TOOLS_PUBLIC
    GENERATE_FILE_PATH  lux/engine/tools/visibility.h
)

# get sublist
subdirectory_list(dir_list)

foreach(subdir ${dir_list})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt)
        add_subdirectory(${subdir})
    endif()
endforeach()

install_projects(
	PROJECT_NAME	lux-engine-tools
	VERSION			0.0.1
	NAMESPACE		lux::engine::tools
	COMPONENTS		builtin_obj
)
