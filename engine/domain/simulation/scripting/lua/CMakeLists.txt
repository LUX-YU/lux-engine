generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_LUA_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_LUA_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_LUA_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/scripting/lua/visibility.h
)
add_component(
    COMPONENT_NAME simulation_script_lua
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script_lua
    SOURCE_FILES src/LuaScriptBackend.cpp
)
component_include_directories(
    simulation_script_lua
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script_lua
    PRIVATE
        LUX_ENGINE_SIMULATION_SCRIPT_LUA_LIBRARY
)
target_link_libraries(
    simulation_script_lua
    PUBLIC
        lux::engine::simulation::simulation_script
        lux::engine::function::script_lua
    PRIVATE LuxLua55::Runtime
)
component_add_transitive_commands(
    simulation_script_lua
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_lua)"
)

if(BUILD_TESTING)
    add_executable(
        lua_script_construction_test
        ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/script-construction/lua.cpp
    )
    target_link_libraries(lua_script_construction_test PRIVATE simulation_script_lua LuxLua55::Runtime)
    if(MSVC)
        target_compile_options(lua_script_construction_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(lua_script_construction_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME script.lua_construction COMMAND lua_script_construction_test)
    set_tests_properties(script.lua_construction PROPERTIES TIMEOUT 30)
endif()
