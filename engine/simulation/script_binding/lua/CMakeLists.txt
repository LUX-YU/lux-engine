find_package(LuaJIT REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_BINDING_LUA_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_BINDING_LUA_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_BINDING_LUA_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/script_binding/lua/visibility.h
)
add_component(
    COMPONENT_NAME simulation_script_binding_lua
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script_binding_lua
    SOURCE_FILES src/LuaScriptBindingBackend.cpp
)
component_include_directories(
    simulation_script_binding_lua
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED ${LUAJIT_INCLUDE_DIRS}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script_binding_lua
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_BINDING_LUA_LIBRARY
)
target_link_libraries(
    simulation_script_binding_lua
    PUBLIC
        lux::engine::simulation::simulation_script_binding
        lux::engine::function::script_lua
    PRIVATE LuaJIT::LuaJIT
)
component_add_transitive_commands(
    simulation_script_binding_lua
    [[list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")]]
    "find_package(LuaJIT REQUIRED)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_binding)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_lua)"
)
lux_classify_target(
    TARGET  simulation_script_binding_lua
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    simulation_script_binding_lua_test
    test/lua_binding_backend_test.cpp
)
target_link_libraries(
    simulation_script_binding_lua_test
    PRIVATE lux::engine::simulation::simulation_script_binding_lua
)
if(MSVC)
    target_compile_options(simulation_script_binding_lua_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_binding_lua_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_binding_lua_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_binding_lua_test
    COMMAND simulation_script_binding_lua_test
)
