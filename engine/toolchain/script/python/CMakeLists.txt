find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_python_importer
    ${CMAKE_CURRENT_SOURCE_DIR}/import_lux_python_script.py
)
add_custom_target(
    toolchain_python_script_importer
    SOURCES ${_python_importer}
)
lux_classify_target(
    TARGET  toolchain_python_script_importer
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

set(_python_fixture_lxsa
    ${CMAKE_CURRENT_BINARY_DIR}/python_behavior_fixture.lxsa
)
add_custom_command(
    OUTPUT ${_python_fixture_lxsa}
    COMMAND
        ${Python3_EXECUTABLE}
        ${_python_importer}
        --source ${CMAKE_CURRENT_SOURCE_DIR}/test/python_behavior_fixture.py
        --output ${_python_fixture_lxsa}
        --semantic-table ${CMAKE_SOURCE_DIR}/modules/function/script/core/include/lux/engine/function/script/ScriptSemanticBuiltin.def
        --module lux.test.python
        --entry EnemyBehavior
        --model ENTITY_BEHAVIOR
    DEPENDS
        ${_python_importer}
        ${CMAKE_CURRENT_SOURCE_DIR}/test/python_behavior_fixture.py
        ${CMAKE_SOURCE_DIR}/modules/function/script/core/include/lux/engine/function/script/ScriptSemanticBuiltin.def
    VERBATIM
)
add_custom_target(
    python_importer_fixture
    DEPENDS ${_python_fixture_lxsa}
)
lux_classify_target(
    TARGET  python_importer_fixture
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)

add_executable(
    python_importer_contract_test
    test/python_importer_contract_test.cpp
)
target_link_libraries(
    python_importer_contract_test
    PRIVATE lux::engine::resource::script_asset
)
target_compile_definitions(
    python_importer_contract_test
    PRIVATE LUX_PYTHON_FIXTURE_LXSA="${_python_fixture_lxsa}"
)
add_dependencies(python_importer_contract_test python_importer_fixture)
if(MSVC)
    target_compile_options(python_importer_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(python_importer_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  python_importer_contract_test
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME python_importer_contract_test
    COMMAND python_importer_contract_test
)

install(
    PROGRAMS ${_python_importer}
    DESTINATION share/lux-engine-toolchain/script/python
)
