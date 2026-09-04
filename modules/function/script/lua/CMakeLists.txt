find_package(lux-cxx REQUIRED COMPONENTS compile_time)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuxLuaVmSelection.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/LuxLuaVmSelection.cmake
    @ONLY
)
include(${CMAKE_CURRENT_BINARY_DIR}/LuxLuaVmSelection.cmake)

include(CheckCXXSourceCompiles)
if(LUX_LUA_VM STREQUAL "LUAJIT")
    set(CMAKE_REQUIRED_LIBRARIES ${LUAJIT_LIBRARIES})
    set(CMAKE_REQUIRED_INCLUDES ${LUAJIT_INCLUDE_DIRS})
    set(_lux_lua_vm_probe [[
        #include <lua.hpp>
        #include <type_traits>
        int main()
        {
            static_assert(std::is_same_v<lua_Number, double>);
            lua_State* state = luaL_newstate();
            if (!state)
                return 1;
            luaL_openlibs(state);
            lua_State* thread = lua_newthread(state);
            const int status = lua_resume(thread, 0);
            (void)status;
            lua_close(state);
            return 0;
        }
    ]])
else()
    set(CMAKE_REQUIRED_LIBRARIES ${LUA_LIBRARIES})
    set(CMAKE_REQUIRED_INCLUDES ${LUA_INCLUDE_DIR})
    set(_lux_lua_vm_probe [[
        #include <lua.hpp>
        #include <type_traits>
        int main()
        {
            static_assert(std::is_same_v<lua_Number, double>);
            lua_State* state = luaL_newstate();
            if (!state)
                return 1;
            luaL_openlibs(state);
            lua_State* thread = lua_newthread(state);
            int results = 0;
            const int status = lua_resume(thread, state, 0, &results);
            (void)status;
            (void)results;
            lua_close(state);
            return 0;
        }
    ]])
endif()
unset(LUX_SELECTED_LUA_VM_CAPABLE CACHE)
check_cxx_source_compiles("${_lux_lua_vm_probe}" LUX_SELECTED_LUA_VM_CAPABLE)
unset(CMAKE_REQUIRED_LIBRARIES)
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
