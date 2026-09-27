# CPU runtime composition, independently usable by a headless game.
find_package(nlohmann_json CONFIG REQUIRED)
set(_runtime_inputs
    lux::engine::scene::scene_world_loading
    lux::engine::scene::scene_mesh_query lux::engine::scene::scene_transform
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
target_link_libraries(builtin_runtime_description PRIVATE ${_runtime_inputs} nlohmann_json::nlohmann_json)
set(_description "${CMAKE_CURRENT_BINARY_DIR}/builtin.declaration.json")
add_custom_command(OUTPUT "${_description}"
    COMMAND "$<TARGET_FILE:builtin_runtime_description>" "${_description}"
    DEPENDS builtin_runtime_description VERBATIM)
add_custom_target(builtin_runtime_declaration DEPENDS "${_description}")
install(TARGETS builtin_runtime_plugin RUNTIME DESTINATION bin LIBRARY DESTINATION lib)

if(TARGET scene_render)
    add_library(builtin_scene_render_exports OBJECT BuiltinSceneRenderExports.cpp)
    target_link_libraries(builtin_scene_render_exports PUBLIC lux::engine::scene::scene_render)
    lux_classify_target(TARGET builtin_scene_render_exports LAYER SCENE PRODUCT RUNTIME ROLE OBJECT)
    add_library(builtin_scene_render_plugin SHARED $<TARGET_OBJECTS:builtin_scene_render_exports>)
    target_link_libraries(builtin_scene_render_plugin PRIVATE lux::engine::scene::scene_render)
    lux_classify_target(TARGET builtin_scene_render_plugin LAYER SCENE PRODUCT RUNTIME ROLE PLUGIN)
    add_executable(builtin_scene_render_description $<TARGET_OBJECTS:builtin_scene_render_exports>
        ${PROJECT_SOURCE_DIR}/cmake/runtime_plugin_description.cpp)
    target_compile_definitions(builtin_scene_render_description PRIVATE LUX_SCENE_RENDER_PLUGIN)
    target_link_libraries(builtin_scene_render_description PRIVATE lux::engine::scene::scene_render
        lux::engine::function::render_features nlohmann_json::nlohmann_json)
    lux_classify_target(TARGET builtin_scene_render_description LAYER TOOLCHAIN PRODUCT SDK ROLE BUILD_TOOL)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/scene-render.declaration.json"
        COMMAND "$<TARGET_FILE:builtin_scene_render_description>" "${CMAKE_CURRENT_BINARY_DIR}/scene-render.declaration.json"
        DEPENDS builtin_scene_render_description VERBATIM)
    add_custom_target(builtin_scene_render_declaration DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/scene-render.declaration.json")
    install(TARGETS builtin_scene_render_plugin RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
endif()
