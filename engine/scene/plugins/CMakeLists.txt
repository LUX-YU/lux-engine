# A single runtime composition; concrete owners keep their existing targets and lifetimes.
find_package(nlohmann_json CONFIG REQUIRED)
set(_runtime_inputs
    lux::engine::scene::scene_render lux::engine::scene::scene_world_loading
    lux::engine::scene::scene_mesh_query lux::engine::simulation::transform_system
    lux::engine::simulation::ecs::hierarchy)
add_library(builtin_runtime_exports OBJECT BuiltinRuntimeExports.cpp)
lux_classify_target(TARGET builtin_runtime_exports LAYER SCENE PRODUCT RUNTIME ROLE OBJECT)
target_link_libraries(builtin_runtime_exports PUBLIC ${_runtime_inputs})
add_library(builtin_runtime_plugin SHARED $<TARGET_OBJECTS:builtin_runtime_exports>)
lux_classify_target(TARGET builtin_runtime_plugin LAYER SCENE PRODUCT RUNTIME ROLE PLUGIN)
target_link_libraries(builtin_runtime_plugin PRIVATE ${_runtime_inputs})
add_executable(builtin_runtime_description $<TARGET_OBJECTS:builtin_runtime_exports>
    ${PROJECT_SOURCE_DIR}/cmake/runtime_plugin_description.cpp)
lux_classify_target(TARGET builtin_runtime_description LAYER TOOLCHAIN PRODUCT SDK ROLE BUILD_TOOL)
target_link_libraries(builtin_runtime_description PRIVATE ${_runtime_inputs} lux::engine::function::render_features nlohmann_json::nlohmann_json)
set(_description "${CMAKE_CURRENT_BINARY_DIR}/builtin.declaration.json")
add_custom_command(OUTPUT "${_description}"
    COMMAND "$<TARGET_FILE:builtin_runtime_description>" "${_description}"
    DEPENDS builtin_runtime_description VERBATIM)
set(_runtime_editor_arguments)
if(TARGET scene_render_meta)
    set(_runtime_editor_arguments EDITOR_TARGET scene_render_meta)
endif()
lux_add_plugin_exports(TARGET builtin_runtime_plugin ${_runtime_editor_arguments} MODULE_ID lux.builtin.runtime DESCRIPTION "${_description}"
    INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/BuiltinRuntimeExports.cpp "${_description}")
install(TARGETS builtin_runtime_plugin RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
