set(_physics2d_source ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/physics2d)

lux_materialize_script_ability_schemas(TARGET physics2d_simulation)
get_target_property(_physics2d_ability_schema_files physics2d_simulation LUX_SCRIPT_ABILITY_SCHEMA_FILES)
get_target_property(_physics2d_ability_schema_target physics2d_simulation LUX_SCRIPT_ABILITY_SCHEMA_TARGET)
add_custom_target(physics2d_installed_schemas ALL DEPENDS ${_physics2d_ability_schema_files})
add_dependencies(physics2d_installed_schemas ${_physics2d_ability_schema_target})
lux_classify_target(TARGET physics2d_installed_schemas LAYER TOOLCHAIN PRODUCT BUILD_TOOL ROLE DOMAIN)
install(
    FILES ${_physics2d_ability_schema_files}
    DESTINATION share/lux-engine-physics2d/schema
    COMPONENT lux_sdk
)

add_executable(physics2d_event_schema_writer ${_physics2d_source}/test/physics2d_event_schema_writer.cpp)
target_include_directories(
    physics2d_event_schema_writer
    PRIVATE ${_physics2d_source}/test
)
target_link_libraries(
    physics2d_event_schema_writer
    PRIVATE
        lux::engine::physics2d::physics2d_simulation
        lux::engine::simulation::simulation_scripting_core
        lux::engine::simulation::simulation_composition
        lux::engine::toolchain::lua_script_packager
)
lux_classify_target(
    TARGET  physics2d_event_schema_writer
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    COMPOSITION
)
set(_physics2d_event_schema ${CMAKE_CURRENT_BINARY_DIR}/physics2d.events.schema.json)
add_custom_command(
    OUTPUT ${_physics2d_event_schema}
    COMMAND $<TARGET_FILE:physics2d_event_schema_writer> ${_physics2d_event_schema}
    DEPENDS physics2d_event_schema_writer
    VERBATIM
)
add_custom_target(physics2d_event_schema DEPENDS ${_physics2d_event_schema})
lux_classify_target(
    TARGET  physics2d_event_schema
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    COMPOSITION
)

set(_physics2d_lua_artifact ${CMAKE_CURRENT_BINARY_DIR}/physics2d_lua_fixture.lxsa)
lux_package_lua_script(
    NAME physics2d_lua_fixture
    SOURCE ${_physics2d_source}/test/physics2d_lua_fixture.lua
    OUTPUT ${_physics2d_lua_artifact}
    ABILITY_TARGETS physics2d_simulation simulation_script
    EVENT_SCHEMAS ${_physics2d_event_schema}
    EVENT_SCHEMA_TARGETS physics2d_event_schema
    SYMBOL_LEDGER ${_physics2d_source}/test/physics2d_lua_fixture.symbols.json
    MODULE lux.physics2d.lua-test
    ENTRY PhysicsBehavior
    SCOPE SIMULATION
)

add_executable(
    physics2d_flowforge_fixture_writer
    ${_physics2d_source}/test/physics2d_flowforge_fixture_writer.cpp
)
target_include_directories(
    physics2d_flowforge_fixture_writer
    PRIVATE ${_physics2d_source}/test
)
target_link_libraries(
    physics2d_flowforge_fixture_writer
    PRIVATE
        lux::engine::physics2d::physics2d_simulation
        lux::engine::flowforge::flowforge_compiler
        lux::engine::simulation::simulation_script
        lux::engine::simulation::simulation_composition
)
lux_classify_target(
    TARGET  physics2d_flowforge_fixture_writer
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    COMPOSITION
)
set(_physics2d_flowforge_artifact ${CMAKE_CURRENT_BINARY_DIR}/physics2d_flowforge_fixture.lxsa)
add_custom_command(
    OUTPUT ${_physics2d_flowforge_artifact}
    COMMAND $<TARGET_FILE:physics2d_flowforge_fixture_writer> ${_physics2d_flowforge_artifact}
    DEPENDS physics2d_flowforge_fixture_writer
    VERBATIM
)
add_custom_target(physics2d_flowforge_fixture DEPENDS ${_physics2d_flowforge_artifact})
lux_classify_target(
    TARGET  physics2d_flowforge_fixture
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    COMPOSITION
)

if(BUILD_TESTING)
    add_executable(
        physics2d_flowforge_integration_test
        ${_physics2d_source}/test/Physics2DFlowForgeTestAbility.cpp
        ${_physics2d_source}/test/physics2d_flowforge_integration_test.cpp
    )
    target_include_directories(
        physics2d_flowforge_integration_test
        PRIVATE ${_physics2d_source}/test
    )
    target_link_libraries(
        physics2d_flowforge_integration_test
        PRIVATE
            lux::engine::physics2d::physics2d_simulation
            lux::engine::flowforge::flowforge_compiler
            lux::engine::simulation::simulation_composition
            lux::engine::simulation::simulation_script
            lux::engine::simulation::simulation_script_native
            lux::engine::core::task
    )
    lux_script_abilities(
        TARGET physics2d_flowforge_integration_test
        SOURCES ${_physics2d_source}/test/Physics2DFlowForgeTestAbility.hpp
        LOGICAL_PATHS Physics2DFlowForgeTestAbility.hpp
    )
    if(MSVC)
        target_compile_options(physics2d_flowforge_integration_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(physics2d_flowforge_integration_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  physics2d_flowforge_integration_test
        LAYER   TOOLCHAIN
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME physics2d_flowforge_integration_test COMMAND physics2d_flowforge_integration_test)
    set_tests_properties(
        physics2d_flowforge_integration_test
        PROPERTIES ENVIRONMENT "LUX_FLOWFORGE_LINKER=${CMAKE_LINKER}"
    )
endif()
