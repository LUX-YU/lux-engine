find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_lua_script_packager ${CMAKE_CURRENT_SOURCE_DIR}/package_lua_script.py)
add_interface_component(
    COMPONENT_NAME lua_script_packager
    NAMESPACE      lux::engine::toolchain
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
