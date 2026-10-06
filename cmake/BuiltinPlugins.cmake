# Runtime product assembly. Frozen Editor contributions are not active providers.
if(TARGET render_features)
    get_target_property(_render_declaration render_features LUX_PLUGIN_DESCRIPTION_INPUT)
    get_target_property(_render_inputs render_features LUX_PLUGIN_SOURCE_INPUTS)
    # Without packed content this target is only the content-provider library;
    # it has no generated feature plugin to publish.
    if(_render_declaration)
        lux_add_plugin_exports(TARGET render_features MODULE_ID lux.builtin.render
            DESCRIPTION "${_render_declaration}" INPUTS ${_render_inputs})
    endif()
endif()

if(TARGET physics2d_simulation)
    get_target_property(_physics_source physics2d_simulation SOURCE_DIR)
    get_target_property(_physics_binary physics2d_simulation BINARY_DIR)
    lux_add_plugin_exports(TARGET physics2d_simulation MODULE_ID lux.builtin.physics2d
        DESCRIPTION "${_physics_binary}/physics2d.declaration.json"
        INPUTS
            ${_physics_source}/src/Physics2DPluginExports.cpp
            ${_physics_source}/src/Physics2DSystem.cpp
            ${_physics_source}/src/Box2DWorld.cpp
            ${_physics_source}/include/lux/engine/physics2d/Physics2DComponents.hpp
            ${_physics_source}/include/lux/engine/physics2d/Physics2DSystem.hpp
            ${_physics_source}/include/lux/engine/physics2d/abilities/PhysicsQuery2D.hpp)
    add_dependencies(physics2d_simulation_plugin_identity physics2d_declaration)
endif()
if(TARGET builtin_runtime_plugin)
    get_target_property(_runtime_source builtin_runtime_plugin SOURCE_DIR)
    get_target_property(_runtime_binary builtin_runtime_plugin BINARY_DIR)
    lux_add_plugin_exports(TARGET builtin_runtime_plugin MODULE_ID lux.builtin.runtime
        DESCRIPTION "${_runtime_binary}/builtin.declaration.json"
        INPUTS ${_runtime_source}/BuiltinRuntimeExports.cpp "${_runtime_binary}/builtin.declaration.json")
    add_dependencies(builtin_runtime_plugin_plugin_identity builtin_runtime_declaration)
endif()
if(TARGET builtin_scene_render_plugin)
    get_target_property(_render_source builtin_scene_render_plugin SOURCE_DIR)
    get_target_property(_render_binary builtin_scene_render_plugin BINARY_DIR)
    lux_add_plugin_exports(TARGET builtin_scene_render_plugin MODULE_ID lux.builtin.scene_render
        DESCRIPTION "${_render_binary}/scene-render.declaration.json"
        INPUTS ${_render_source}/BuiltinSceneRenderExports.cpp "${_render_binary}/scene-render.declaration.json")
    add_dependencies(builtin_scene_render_plugin_plugin_identity builtin_scene_render_declaration)
endif()

