include_component_cmake_scripts(meta)

add_executable(
	engine_script_test
	${CMAKE_CURRENT_SOURCE_DIR}/meta_test.cpp
)

target_link_libraries(
	engine_script_test
	PRIVATE
	lux::engine::core::script
)

engine_add_meta(
	NAME engine_test_meta
	TARGET_FILES
		${CMAKE_CURRENT_SOURCE_DIR}/include/test_target_file.hpp
	REGISTER_FUNC_NAME
		lux_engine_meta_func_meta_test
	# CUSTOM_INCLUDE
	#     "lux/engine/core/visibility.h"
	# REGISTER_FUNC_MACRO
	#     LUX_CORE_PUBLIC
)

engine_add_lua_binding(
	NAME      engine_test_meta_lua
	FROM_META engine_test_meta
)

engine_target_add_meta(
	TARGET	engine_script_test
    METAS	engine_test_meta
	OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/include
)

engine_target_add_lua_binding(
	TARGET       engine_script_test
	LUA_BINDINGS engine_test_meta_lua
	OUT_DIR      ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_include_directories(
	engine_script_test
	PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${ENTT_INCLUDE_DIRS}
)