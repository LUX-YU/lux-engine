set(_physics2d_source ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/physics2d)

lux_materialize_script_ability_schemas(TARGET physics2d_simulation)
get_target_property(_physics2d_ability_schema_files physics2d_simulation LUX_SCRIPT_ABILITY_SCHEMA_FILES)
get_target_property(_physics2d_ability_schema_target physics2d_simulation LUX_SCRIPT_ABILITY_SCHEMA_TARGET)
add_custom_target(physics2d_installed_schemas ALL DEPENDS ${_physics2d_ability_schema_files})
add_dependencies(physics2d_installed_schemas ${_physics2d_ability_schema_target})

install(
    FILES ${_physics2d_ability_schema_files}
    DESTINATION share/lux-engine-physics2d/schema
    COMPONENT lux_sdk
)
