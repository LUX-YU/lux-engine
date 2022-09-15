# INCLUDE GUARD
if(_MODULE_TEST_INCLUDED_)
	return()
endif()
set(_MODULE_TEST_INCLUDED_ TRUE)


function(module_test)
	set(_options	  			)
	set(_one_value_arguments	EXECUTABLE_NAME)
	set(_multi_value_arguments	SOURCE_FILES DEPENDENT_TARGETS)
	
	cmake_parse_arguments(
		MODULE_TEST_ARGS
		"${_options}"
		"${_one_value_arguments}"
		"${_multi_value_arguments}"
		${ARGN}
	)
		
	add_executable(
		${MODULE_TEST_ARGS_EXECUTABLE_NAME}
		${MODULE_TEST_ARGS_SOURCE_FILES}
	)

	target_link_libraries(
		${MODULE_TEST_ARGS_EXECUTABLE_NAME}
		PRIVATE
		${MODULE_TEST_ARGS_DEPENDENT_TARGETS}
	)
endfunction()
