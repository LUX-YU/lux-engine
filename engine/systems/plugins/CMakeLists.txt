# CPU runtime composition, independently usable by a headless game.
find_package(nlohmann_json CONFIG REQUIRED)
set(_runtime_inputs
    lux::engine::scene::system_world_streaming
    lux::engine::scene::system_mesh_query lux::engine::scene::system_transform
    lux::engine::ecs::ecs_hierarchy)
add_library(builtin_runtime_exports OBJECT BuiltinRuntimeExports.cpp)
lux_classify_target(TARGET builtin_runtime_exports LAYER SYSTEMS PRODUCT RUNTIME ROLE OBJECT)
target_link_libraries(builtin_runtime_exports PUBLIC ${_runtime_inputs})
add_library(builtin_runtime_plugin SHARED $<TARGET_OBJECTS:builtin_runtime_exports>)
lux_classify_target(TARGET builtin_runtime_plugin LAYER SYSTEMS PRODUCT RUNTIME ROLE PLUGIN)
target_link_libraries(builtin_runtime_plugin PRIVATE ${_runtime_inputs})
add_executable(builtin_runtime_description $<TARGET_OBJECTS:builtin_runtime_exports>
    ${PROJECT_SOURCE_DIR}/cmake/runtime_plugin_description.cpp)
lux_classify_target(TARGET builtin_runtime_description LAYER TOOLCHAIN PRODUCT SDK ROLE BUILD_TOOL)
target_link_libraries(builtin_runtime_description PRIVATE ${_runtime_inputs} nlohmann_json::nlohmann_json)
set(_description "${CMAKE_CURRENT_BINARY_DIR}/builtin.declaration.json")
add_custom_command(OUTPUT "${_description}"
    COMMAND "$<TARGET_FILE:builtin_runtime_description>" "${_description}"
    DEPENDS builtin_runtime_description VERBATIM)
add_custom_target(builtin_runtime_declaration DEPENDS "${_description}")
install(TARGETS builtin_runtime_plugin RUNTIME DESTINATION bin LIBRARY DESTINATION lib)

if(TARGET system_render)
    add_library(builtin_scene_render_exports OBJECT BuiltinSceneRenderExports.cpp)
    target_link_libraries(
        builtin_scene_render_exports
        PUBLIC lux::engine::systems::system_render lux::engine::scene::scene_view
    )
    lux_classify_target(TARGET builtin_scene_render_exports LAYER SYSTEMS PRODUCT RUNTIME ROLE OBJECT)
    add_library(builtin_scene_render_plugin SHARED $<TARGET_OBJECTS:builtin_scene_render_exports>)
    target_link_libraries(
        builtin_scene_render_plugin
        PRIVATE lux::engine::systems::system_render lux::engine::scene::scene_view
    )
    lux_classify_target(TARGET builtin_scene_render_plugin LAYER SYSTEMS PRODUCT RUNTIME ROLE PLUGIN)
    add_executable(
        builtin_scene_render_description
        $<TARGET_OBJECTS:builtin_scene_render_exports>
        ${PROJECT_SOURCE_DIR}/cmake/runtime_plugin_description.cpp
    )
    target_link_libraries(
        builtin_scene_render_description
        PRIVATE lux::engine::systems::system_render lux::engine::scene::scene_view nlohmann_json::nlohmann_json
    )
    lux_classify_target(TARGET builtin_scene_render_description LAYER TOOLCHAIN PRODUCT SDK ROLE BUILD_TOOL)
    set(render_description "${CMAKE_CURRENT_BINARY_DIR}/scene-render.declaration.json")
    add_custom_command(
        OUTPUT "${render_description}"
        COMMAND "$<TARGET_FILE:builtin_scene_render_description>" "${render_description}"
            lux.builtin.scene_render "Asset-backed Scene projection through typed Render commands."
            lux.builtin.runtime 2
        DEPENDS builtin_scene_render_description
        VERBATIM
    )
    add_custom_target(builtin_scene_render_declaration DEPENDS "${render_description}")
    install(TARGETS builtin_scene_render_plugin RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
endif()
