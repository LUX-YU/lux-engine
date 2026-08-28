find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_lua_script_packager ${CMAKE_CURRENT_SOURCE_DIR}/package_lua_script.py)
set(_script_symbol_ledger
    ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_behavior_fixture.symbols.json
)
add_custom_target(lua_script_packager SOURCES ${_lua_script_packager})
lux_classify_target(
    TARGET  lua_script_packager
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

set(_lua_fixture_lxsa ${CMAKE_CURRENT_BINARY_DIR}/lua_behavior_fixture.lxsa)
add_custom_command(
    OUTPUT ${_lua_fixture_lxsa}
    COMMAND
        ${Python3_EXECUTABLE}
        ${_lua_script_packager}
        --source ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_behavior_fixture.lua
        --output ${_lua_fixture_lxsa}
        --record-type lux.simulation.SimulationStepInfo,16,8
        --record-type lux.test.CollisionEvent,8,4
        --symbol-ledger ${_script_symbol_ledger}
        --module lux.test.lua
        --entry EnemyBehavior
        --scope ENTITY
    DEPENDS
        ${_lua_script_packager}
        ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_behavior_fixture.lua
        ${_script_symbol_ledger}
    VERBATIM
)
add_custom_target(lua_packager_fixture DEPENDS ${_lua_fixture_lxsa})
lux_classify_target(
    TARGET  lua_packager_fixture
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)

add_executable(
    lua_script_packager_contract_test
    test/lua_script_packager_contract_test.cpp
)
target_link_libraries(
    lua_script_packager_contract_test
    PRIVATE lux::engine::function::script_artifact
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

install(
    PROGRAMS ${_lua_script_packager}
    DESTINATION share/lux-engine-toolchain/lua
)
