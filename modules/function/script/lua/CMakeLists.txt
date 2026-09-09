find_package(lux-cxx REQUIRED COMPONENTS compile_time)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuxLuaVmSelection.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/LuxLuaVmSelection.cmake
    @ONLY
)
include(${CMAKE_CURRENT_BINARY_DIR}/LuxLuaVmSelection.cmake)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES lux::engine::function::script_lua_vm)
if(LUX_LUA_VM STREQUAL "LUAJIT")
    set(_lux_expected_lua_version 501)
elseif(LUX_LUA_VM STREQUAL "LUA54")
    set(_lux_expected_lua_version 504)
else()
    set(_lux_expected_lua_version 505)
endif()
set(CMAKE_REQUIRED_DEFINITIONS -DLUX_EXPECTED_LUA_VERSION=${_lux_expected_lua_version})
set(_lux_lua_vm_probe [[
    #include <lua.hpp>
    #include <type_traits>
    static_assert(LUA_VERSION_NUM == LUX_EXPECTED_LUA_VERSION);
    static_assert(std::is_same_v<lua_Number, double>);
    #if LUX_EXPECTED_LUA_VERSION == 505
    static_assert(LUA_VERSION_RELEASE_NUM == 50501);
    static_assert(std::is_signed_v<lua_Integer> && sizeof(lua_Integer) == 8);
    #endif
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
        #if LUX_EXPECTED_LUA_VERSION == 501
        if (lua_resume(thread, 0) != LUA_YIELD) return 4;
        if (lua_resume(thread, 0) != LUA_OK || lua_gettop(thread) != 1) return 5;
        #else
        int results{};
        if (lua_resume(thread, state, 0, &results) != LUA_YIELD || results != 0) return 4;
        if (lua_resume(thread, state, 0, &results) != LUA_OK || results != 1) return 5;
        #endif
        if (lua_tonumber(thread, -1) != 42.0) return 6;
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
        lua_close(state);
        return 0;
    }
]])
unset(LUX_SELECTED_LUA_VM_CAPABLE CACHE)
check_cxx_source_compiles("${_lux_lua_vm_probe}" LUX_SELECTED_LUA_VM_CAPABLE)
unset(CMAKE_REQUIRED_LIBRARIES)
unset(CMAKE_REQUIRED_DEFINITIONS)
unset(_lux_expected_lua_version)
unset(CMAKE_REQUIRED_INCLUDES)
unset(_lux_lua_vm_probe)
if(NOT LUX_SELECTED_LUA_VM_CAPABLE)
    message(FATAL_ERROR
        "Selected Lua VM '${LUX_LUA_VM}' does not provide the required Lux C API or double lua_Number contract"
    )
endif()

add_component(
    COMPONENT_NAME  script_lua
    NAMESPACE       lux::engine::function
    OUTPUT_NAME     lux_engine_function_script_lua
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Lua.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/LuaVm.cpp
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
        LUX_SCRIPT_LUA_VM_${LUX_LUA_VM}=1
)

target_link_libraries(
    script_lua
    PUBLIC
        lux::engine::function::script_core
        lux::cxx::compile_time
    PRIVATE
        lux::engine::function::script_lua_vm
)
get_target_property(_lux_lua_vm_runtime lux::engine::function::script_lua_vm LUX_LUA_VM_RUNTIME_FILE)
if(_lux_lua_vm_runtime)
    add_custom_command(
        TARGET script_lua
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_lux_lua_vm_runtime}"
            "$<TARGET_FILE_DIR:script_lua>"
        VERBATIM
    )
    install(
        FILES "${_lux_lua_vm_runtime}"
        DESTINATION bin
        COMPONENT lux_sdk
    )
endif()
unset(_lux_lua_vm_runtime)

component_add_transitive_commands(
    script_lua
    [[include("${CMAKE_CURRENT_LIST_DIR}/LuxLuaVmSelection.cmake")]]
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

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/LuxLuaVmSelection.cmake
        ${CMAKE_SOURCE_DIR}/cmake/FindLuaJIT.cmake
    DESTINATION share/lux-engine-function/script_lua
)

lux_classify_target(
    TARGET  script_lua
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

set(ENABLE_SCRIPT_LUA_TEST OFF CACHE BOOL "Enable Lua script tests")
if(ENABLE_SCRIPT_LUA_TEST)
    add_subdirectory(test)
endif()
