find_package(LuaJIT REQUIRED)

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
    BUILD_TIME_SHARED ${LUAJIT_INCLUDE_DIRS}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script_lua
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_LUA_LIBRARY
)
target_link_libraries(
    simulation_script_lua
    PUBLIC
        lux::engine::simulation::simulation_script
        lux::engine::function::script_lua
    PRIVATE LuaJIT::LuaJIT
)
component_add_transitive_commands(
    simulation_script_lua
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_lua)"
)
lux_classify_target(
    TARGET  simulation_script_lua
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    simulation_script_lua_test
    test/lua_backend_test.cpp
)
target_link_libraries(
    simulation_script_lua_test
    PRIVATE
        lux::engine::simulation::simulation_script_lua
        LuaJIT::LuaJIT
)
if(MSVC)
    target_compile_options(simulation_script_lua_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lua_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_lua_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_lua_test
    COMMAND simulation_script_lua_test
)

add_executable(
    simulation_script_luajit_coroutine_contract_test
    test/luajit_coroutine_contract_test.cpp
)
target_link_libraries(
    simulation_script_luajit_coroutine_contract_test
    PRIVATE LuaJIT::LuaJIT
)
if(MSVC)
    target_compile_options(simulation_script_luajit_coroutine_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_luajit_coroutine_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_luajit_coroutine_contract_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_luajit_coroutine_contract_test
    COMMAND simulation_script_luajit_coroutine_contract_test
)

add_executable(
    simulation_script_lua_coroutine_integration_test
    test/lua_coroutine_integration_test.cpp
)
target_link_libraries(
    simulation_script_lua_coroutine_integration_test
    PRIVATE lux::engine::simulation::simulation_script_lua
)
target_include_directories(
    simulation_script_lua_coroutine_integration_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
)
include_component_cmake_scripts(script_core)
lux_script_abilities(
    TARGET simulation_script_lua_coroutine_integration_test
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test/LuaRuntimeTestAbility.hpp
    LOGICAL_PATHS LuaRuntimeTestAbility.hpp
)
if(MSVC)
    target_compile_options(simulation_script_lua_coroutine_integration_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lua_coroutine_integration_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_lua_coroutine_integration_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_lua_coroutine_integration_test
    COMMAND simulation_script_lua_coroutine_integration_test
)

if(TARGET script_native_collision_fixture)
    add_executable(
        simulation_script_collision_event_test
        test/collision_event_chain_test.cpp
    )
    target_link_libraries(
        simulation_script_collision_event_test
        PRIVATE
            lux::engine::simulation::simulation_script_lua
            lux::engine::simulation::simulation_script_native
            LuaJIT::LuaJIT
    )
    target_compile_definitions(
        simulation_script_collision_event_test
        PRIVATE
            LUX_SCRIPT_NATIVE_COLLISION_FIXTURE="$<TARGET_FILE:script_native_collision_fixture>"
    )
    add_dependencies(
        simulation_script_collision_event_test
        script_native_collision_fixture
    )
    if(MSVC)
        target_compile_options(
            simulation_script_collision_event_test
            PRIVATE /UNDEBUG
        )
    else()
        target_compile_options(
            simulation_script_collision_event_test
            PRIVATE -UNDEBUG
        )
    endif()
    lux_classify_target(
        TARGET  simulation_script_collision_event_test
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    DOMAIN
    )
    add_test(
        NAME simulation_script_collision_event_test
        COMMAND simulation_script_collision_event_test
    )
endif()
