# INCLUDE GUARD
if(_ADD_TOOL_MODULE_INCLUDED_)
	return()
endif()
set(_ADD_TOOL_MODULE_INCLUDED_ TRUE)

function(add_header_only_module)


endfunction()

function(add_module)
	set(_options	  			STATIC)
	set(_one_value_arguments	MODULE_NAME NAMESPACE)
	set(_multi_value_arguments
		SOURCE_FILES

		EXPORT_INCLUDE_DIRS
		PROJECT_SHARED_INCLUDE_DIRS
		PRIVATE_INCLUDE_DIRS

		PUBLIC_LIBRARIES
		PRIVATE_LIBRARIES
	)

	cmake_parse_arguments(
		MODULE_ARGS
		"${_options}"
		"${_one_value_arguments}"
		"${_multi_value_arguments}"
		${ARGN}
	)

	message("-- MODULE NAME:${MODULE_ARGS_MODULE_NAME}")
	
	# check module name
	if(NOT MODULE_ARGS_MODULE_NAME)
		message(FATAL_ERROR "Module name not specified.")
	endif()

	# check namespace
	if(NOT MODULE_ARGS_NAMESPACE)
		message(FATAL_ERROR "Module namespace not specified.")
	endif()

	# no source files
	if(NOT MODULE_ARGS_SOURCE_FILES) # don't have any source file
		message(FATAL_ERROR "Source files didn't detected, use add_header_only_module instead.")
	endif()

	if(MODULE_ARGS_STATIC)
		set(LIBRARY_TYPE	STATIC)
	else()
		set(LIBRARY_TYPE	SHARED)
	endif()

	add_library(
		${MODULE_ARGS_MODULE_NAME}
		${LIBRARY_TYPE}
		${MODULE_ARGS_SOURCE_FILES}
	)

	# alias
	set(ALIAS_NAME lux::engine::${MODULE_ARGS_NAMESPACE}::${MODULE_ARGS_MODULE_NAME})
	message("---- ALIAS NAME:${ALIAS_NAME}")
	add_library(
		${ALIAS_NAME}
		ALIAS 
		${MODULE_ARGS_MODULE_NAME}
	)

	set_target_properties(
		${MODULE_ARGS_MODULE_NAME}
		PROPERTIES 
		OUTPUT_NAME lux_engine_${MODULE_ARGS_MODULE_NAME}
	)

	target_link_libraries(
		${MODULE_ARGS_MODULE_NAME}
		PUBLIC
			${MODULE_ARGS_PUBLIC_LIBRARIES}
		PRIVATE
			${MODULE_ARGS_PRIVATE_LIBRARIES}
	)

	foreach(export_include_dir ${MODULE_ARGS_EXPORT_INCLUDE_DIRS})
		message("---- Module export include dir:${export_include_dir}")
		target_include_directories(
			${MODULE_ARGS_MODULE_NAME}
			PUBLIC
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${export_include_dir}>
			$<INSTALL_INTERFACE:${export_include_dir}>
		)
	endforeach()

	foreach(shared_include_dir ${MODULE_ARGS_PROJECT_SHARED_INCLUDE_DIRS})
		message("---- Module shared include dir:${shared_include_dir}")
		target_include_directories(
			${MODULE_ARGS_MODULE_NAME}
			PUBLIC
			$<BUILD_INTERFACE:${shared_include_dir}>
		)
	endforeach()

	foreach(private_include_dir ${MODULE_ARGS_PRIVATE_INCLUDE_DIRS})
		message("---- Module private include dir:${private_include_dir}")
		target_include_directories(
			${MODULE_ARGS_MODULE_NAME}
			PRIVATE
			${private_include_dir}
		)
	endforeach()

	install(
		DIRECTORY	${MODULE_ARGS_EXPORT_INCLUDE_DIRS}
		DESTINATION ${CMAKE_INSTALL_PREFIX}
	)

	install(
		TARGETS	${MODULE_ARGS_MODULE_NAME}
		EXPORT lux::engine::${MODULE_ARGS_NAMESPACE}
	)
endfunction()
