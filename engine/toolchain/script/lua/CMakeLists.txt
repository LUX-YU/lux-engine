find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_lua_importer ${CMAKE_CURRENT_SOURCE_DIR}/import_lux_lua_script.py)
set(_script_semantic_catalog
    ${CMAKE_SOURCE_DIR}/engine/authoring/script_binding/assets/lux-script-semantics-project-fixture-v1.json
)
add_custom_target(toolchain_lua_script_importer SOURCES ${_lua_importer})
lux_classify_target(
    TARGET  toolchain_lua_script_importer
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

set(_lua_fixture_lxsa ${CMAKE_CURRENT_BINARY_DIR}/lua_behavior_fixture.lxsa)
add_custom_command(
    OUTPUT ${_lua_fixture_lxsa}
    COMMAND
        ${Python3_EXECUTABLE}
        ${_lua_importer}
        --source ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_behavior_fixture.lua
        --output ${_lua_fixture_lxsa}
        --semantic-catalog ${_script_semantic_catalog}
        --module lux.test.lua
        --entry EnemyBehavior
        --model ENTITY_BEHAVIOR
    DEPENDS
        ${_lua_importer}
        ${CMAKE_CURRENT_SOURCE_DIR}/test/lua_behavior_fixture.lua
        ${_script_semantic_catalog}
    VERBATIM
)
add_custom_target(lua_importer_fixture DEPENDS ${_lua_fixture_lxsa})
lux_classify_target(
    TARGET  lua_importer_fixture
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)

add_executable(
    lua_importer_contract_test
    test/lua_importer_contract_test.cpp
)
target_link_libraries(
    lua_importer_contract_test
    PRIVATE lux::engine::resource::script_asset
)
target_compile_definitions(
    lua_importer_contract_test
    PRIVATE LUX_LUA_FIXTURE_LXSA="${_lua_fixture_lxsa}"
)
add_dependencies(lua_importer_contract_test lua_importer_fixture)
if(MSVC)
    target_compile_options(lua_importer_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(lua_importer_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  lua_importer_contract_test
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME lua_importer_contract_test COMMAND lua_importer_contract_test)

install(
    PROGRAMS ${_lua_importer}
    DESTINATION share/lux-engine-toolchain/script/lua
)
