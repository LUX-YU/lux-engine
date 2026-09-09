cmake_minimum_required(VERSION 3.22)
project(LuxLua55 VERSION 5.5.1 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Lua55 qualification requires a single RelWithDebInfo configuration")
endif()
set(LUX_LUA55_SOURCE_DIR "" CACHE PATH "Absolute path to the verified official Lua 5.5.1 source")
if(NOT IS_ABSOLUTE "${LUX_LUA55_SOURCE_DIR}" OR NOT EXISTS "${LUX_LUA55_SOURCE_DIR}/src/lua.h")
    message(FATAL_ERROR "Provide the verified Lua 5.5.1 source with LUX_LUA55_SOURCE_DIR; no automatic fetch")
endif()
file(READ "${LUX_LUA55_SOURCE_DIR}/src/lua.h" lua_header)
foreach(part MAJOR MINOR RELEASE)
    string(REGEX MATCH "#define[ \t]+LUA_VERSION_${part}_N[ \t]+([0-9]+)" match "${lua_header}")
    set(lua_${part} "${CMAKE_MATCH_1}")
endforeach()
if(NOT "${lua_MAJOR}.${lua_MINOR}.${lua_RELEASE}" STREQUAL "5.5.1")
    message(FATAL_ERROR "The dependency source must be exactly Lua 5.5.1")
endif()

# Explicit library sources: no CLI, compiler main, amalgamation, or VM modifications.
set(lua_sources
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c ldump.c
    lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c
    loslib.c lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
    lutf8lib.c lvm.c lzio.c
)
list(TRANSFORM lua_sources PREPEND "${LUX_LUA55_SOURCE_DIR}/src/")
add_library(lux_lua55_runtime SHARED ${lua_sources})
add_library(LuxLua55::Runtime ALIAS lux_lua55_runtime)
set_target_properties(lux_lua55_runtime PROPERTIES EXPORT_NAME Runtime OUTPUT_NAME lux_lua55
    MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
target_include_directories(lux_lua55_runtime PUBLIC
    "$<BUILD_INTERFACE:${LUX_LUA55_SOURCE_DIR}/src>" "$<INSTALL_INTERFACE:include/lua55>")
if(WIN32)
    target_compile_definitions(lux_lua55_runtime PUBLIC LUA_BUILD_AS_DLL)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(lux_lua55_runtime PRIVATE LUA_USE_LINUX)
    target_link_libraries(lux_lua55_runtime PRIVATE m ${CMAKE_DL_LIBS})
else()
    message(FATAL_ERROR "This dependency wrapper has no qualified platform recipe for ${CMAKE_SYSTEM_NAME}")
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(LuxLua55Config.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/LuxLua55Config.cmake"
    INSTALL_DESTINATION lib/cmake/LuxLua55)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/LuxLua55ConfigVersion.cmake"
    VERSION 5.5.1 COMPATIBILITY ExactVersion)
install(TARGETS lux_lua55_runtime EXPORT LuxLua55Targets RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(EXPORT LuxLua55Targets NAMESPACE LuxLua55:: DESTINATION lib/cmake/LuxLua55)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LuxLua55Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/LuxLua55ConfigVersion.cmake" DESTINATION lib/cmake/LuxLua55)
foreach(header lua.h luaconf.h lauxlib.h lualib.h lua.hpp)
    install(FILES "${LUX_LUA55_SOURCE_DIR}/src/${header}" DESTINATION include/lua55)
endforeach()
if(MSVC)
    install(FILES "$<TARGET_PDB_FILE:lux_lua55_runtime>" DESTINATION bin OPTIONAL)
endif()

enable_testing()
add_executable(lua55_smoke smoke.c)
target_link_libraries(lua55_smoke PRIVATE LuxLua55::Runtime)
set_target_properties(lua55_smoke PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
add_test(NAME lua55_c_api_smoke COMMAND lua55_smoke)
