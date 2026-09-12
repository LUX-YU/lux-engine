find_package(lux-cxx REQUIRED COMPONENTS compile_time)

find_package(LuxLua55 5.5.1 EXACT CONFIG REQUIRED)
enable_language(C)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES LuxLua55::Runtime)
set(_lux_lua_vm_probe [[
    #include <lua.hpp>
    #include <type_traits>
    static_assert(LUA_VERSION_NUM == 505);
    static_assert(std::is_same_v<lua_Number, double>);
    static_assert(LUA_VERSION_RELEASE_NUM == 50501);
    static_assert(std::is_signed_v<lua_Integer> && sizeof(lua_Integer) == 8);
    int main()
    {
        lua_State* state = luaL_newstate();
        if (!state) return 1;
        luaL_openlibs(state);
        if (luaL_loadstring(state, "return function() coroutine.yield(); return 42 end") != LUA_OK) return 2;
        if (lua_pcall(state, 0, 1, 0) != LUA_OK) return 3;
        lua_State* thread = lua_newthread(state);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_xmove(state, thread, 1);
        int results{};
        if (lua_resume(thread, state, 0, &results) != LUA_YIELD || results != 0) return 4;
        if (lua_resume(thread, state, 0, &results) != LUA_OK || results != 1) return 5;
        if (lua_tonumber(thread, -1) != 42.0) return 6;
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
        lua_close(state);
        return 0;
    }
]])
unset(LUX_LUA55_CAPABLE CACHE)
check_cxx_source_compiles("${_lux_lua_vm_probe}" LUX_LUA55_CAPABLE)
unset(CMAKE_REQUIRED_LIBRARIES)
unset(CMAKE_REQUIRED_DEFINITIONS)
unset(CMAKE_REQUIRED_INCLUDES)
unset(_lux_lua_vm_probe)
if(NOT LUX_LUA55_CAPABLE)
    message(FATAL_ERROR
        "Lua 5.5.1 does not provide the required Lux C API or double lua_Number contract"
    )
endif()

add_component(
    COMPONENT_NAME  script_lua
    NAMESPACE       lux::engine::function
    OUTPUT_NAME     lux_engine_function_script_lua
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Lua.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/LuaVm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/LuaBoundary.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/LuaValue.cpp
)

component_include_directories(
    script_lua
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME
        include
    PRIVATE
        pinclude
)

target_compile_definitions(
    script_lua
    PRIVATE
        LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    script_lua
    PUBLIC
        lux::engine::function::script_core
        lux::cxx::compile_time
    PRIVATE
        LuxLua55::Runtime
)
include(CheckIPOSupported)
check_ipo_supported(RESULT _lux_lua_ipo_supported OUTPUT _lux_lua_ipo_error LANGUAGES CXX)
if(_lux_lua_ipo_supported)
    set_property(TARGET script_lua PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
add_custom_command(TARGET script_lua POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:LuxLua55::Runtime>" "$<TARGET_FILE_DIR:script_lua>"
    VERBATIM
)
install(FILES "$<TARGET_FILE:LuxLua55::Runtime>" DESTINATION bin COMPONENT lux_sdk)

component_add_transitive_commands(
    script_lua
    "find_package(LuxLua55 5.5.1 EXACT CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
)

component_add_cmake_scripts(
    script_lua
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_lua_codegen.cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_lua_values.cmake
)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cmake/template/
    DESTINATION share/lux-engine-function/script_lua/cmake_scripts/template
)
