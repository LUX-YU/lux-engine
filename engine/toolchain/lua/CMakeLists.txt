find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_lua_script_packager ${CMAKE_CURRENT_SOURCE_DIR}/package_lua_script.py)
add_interface_component(
    COMPONENT_NAME lua_script_packager
    NAMESPACE      lux::engine::toolchain
)
component_include_directories(
    lua_script_packager
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_link_libraries(
    lua_script_packager
    INTERFACE lux::engine::function::script_core
)
component_add_transitive_commands(
    lua_script_packager
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
)
lux_classify_target(
    TARGET  lua_script_packager
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)
component_add_cmake_scripts(
    lua_script_packager
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux_package_lua_script.cmake
)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux_package_lua_script.cmake)

add_executable(lua_event_schema_writer test/lua_event_schema_writer.cpp)
target_link_libraries(lua_event_schema_writer PRIVATE lua_script_packager)
lux_classify_target(
    TARGET  lua_event_schema_writer
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

set(_lua_gameplay_event_schema ${CMAKE_CURRENT_BINARY_DIR}/lua_gameplay.events.schema.json)
add_custom_command(
    OUTPUT ${_lua_gameplay_event_schema}
    COMMAND $<TARGET_FILE:lua_event_schema_writer> ${_lua_gameplay_event_schema} gameplay
    DEPENDS lua_event_schema_writer
    VERBATIM
)
add_custom_target(lua_gameplay_event_schema DEPENDS ${_lua_gameplay_event_schema})
lux_classify_target(
    TARGET  lua_gameplay_event_schema
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

set(_lua_benchmark_event_schema ${CMAKE_CURRENT_BINARY_DIR}/lua_benchmark.events.schema.json)
add_custom_command(
    OUTPUT ${_lua_benchmark_event_schema}
    COMMAND $<TARGET_FILE:lua_event_schema_writer> ${_lua_benchmark_event_schema} benchmark
    DEPENDS lua_event_schema_writer
    VERBATIM
)
add_custom_target(lua_benchmark_event_schema DEPENDS ${_lua_benchmark_event_schema})
lux_classify_target(
    TARGET  lua_benchmark_event_schema
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

set(_lua_portability_lxsa ${CMAKE_CURRENT_BINARY_DIR}/lua_portability_fixture.lxsa)
lux_package_lua_script(
    NAME lua_portability_fixture
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_portability_fixture.lua
    OUTPUT ${_lua_portability_lxsa}
    VALUE_TYPES lux.simulation.ScriptEndPlayReason,3,4,4
    ABILITY_TARGETS simulation_script script_lua_portability_ability_contract
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_portability_fixture.symbols.json
    MODULE lux.test.lua-portability
    ENTRY LuaPortabilityFixture
    SCOPE SIMULATION
)

set(_lua_fixture_lxsa ${CMAKE_CURRENT_BINARY_DIR}/lua_behavior_fixture.lxsa)
lux_package_lua_script(
    NAME lua_packager_fixture
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_behavior_fixture.lua
    OUTPUT ${_lua_fixture_lxsa}
    RECORD_TYPES
        lux.simulation.SimulationStepInfo,16,8
        lux.test.CollisionEvent,8,4
    VALUE_TYPES lux.simulation.ScriptEndPlayReason,3,4,4
    ABILITY_TARGETS simulation_script
    EVENT_SCHEMAS ${_lua_gameplay_event_schema}
    EVENT_SCHEMA_TARGETS lua_gameplay_event_schema
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_behavior_fixture.symbols.json
    MODULE lux.test.lua
    ENTRY EnemyBehavior
    SCOPE ENTITY
)

set(_lua_benchmark_lxsa ${CMAKE_CURRENT_BINARY_DIR}/lua_runtime_benchmark_fixture.lxsa)
lux_package_lua_script(
    NAME lua_runtime_benchmark_fixture
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_runtime_benchmark_fixture.lua
    OUTPUT ${_lua_benchmark_lxsa}
    VALUE_TYPES lux.simulation.ScriptEndPlayReason,3,4,4
    ABILITY_TARGETS simulation_script script_benchmark_ability_contract
    EVENT_SCHEMAS ${_lua_benchmark_event_schema}
    EVENT_SCHEMA_TARGETS lua_benchmark_event_schema
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_runtime_benchmark_fixture.symbols.json
    MODULE lux.benchmark.lua-runtime
    ENTRY BenchmarkBehavior
    SCOPE ENTITY
)

add_executable(
    lua_script_packager_contract_test
    test/lua_script_packager_contract_test.cpp
)
target_link_libraries(
    lua_script_packager_contract_test
    PRIVATE
        lux::engine::function::script_artifact
        lux::engine::simulation::simulation_script
)
target_compile_definitions(
    lua_script_packager_contract_test
    PRIVATE LUX_LUA_FIXTURE_LXSA="${_lua_fixture_lxsa}"
)
add_dependencies(lua_script_packager_contract_test lua_packager_fixture)
if(MSVC)
    target_compile_options(lua_script_packager_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(lua_script_packager_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  lua_script_packager_contract_test
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME lua_script_packager_contract_test COMMAND lua_script_packager_contract_test)
add_test(
    NAME lua_script_packager_negative_test
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_script_packager_negative_test.py
        ${_lua_script_packager}
)

install(
    PROGRAMS ${_lua_script_packager}
    DESTINATION share/lux-engine-toolchain/lua_script_packager/cmake_scripts
)

install_components(
    PROJECT_NAME lux-engine-toolchain
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::toolchain
    COMPONENTS   lua_script_packager
)
