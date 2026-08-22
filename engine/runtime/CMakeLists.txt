## Runtime product layer.  Subdirectories are ordered by dependency, not by
## deployment unit: source boundaries remain explicit even when components are
## linked statically into a host.

add_subdirectory(execution)
add_subdirectory(assets)
add_subdirectory(extensions)
add_subdirectory(logging)
add_subdirectory(entity_scene)
add_subdirectory(assets/pixel)
add_subdirectory(assets/navigation)
add_subdirectory(spatial3d/partitioned)
add_subdirectory(spatial3d/physics)
add_subdirectory(scene/core)
add_subdirectory(scene/composition)
add_subdirectory(render/scene)
add_subdirectory(render/backend_host)
add_subdirectory(scene/script)
add_subdirectory(frame)

# This vertical integration test needs SceneRuntime, whose target is declared
# after the dimension-specific production leaves above.
add_subdirectory(spatial3d/partitioned/test)

install_components(
    PROJECT_NAME    lux-engine-runtime
    VERSION         0.0.1
    NAMESPACE       lux::engine::runtime
    COMPONENTS      runtime_execution
                    runtime_assets
                    runtime_extension_loader
                    runtime_extension_orchestration
                    runtime_logging
                    runtime_entity_scene
                    runtime_tilemap_prepare
                    runtime_pixel_assets
                    runtime_navigation3d_prepare
                    runtime_spatial3d_streaming_systems
                    runtime_spatial3d_physics
                    runtime_scene_core
                    runtime_tilemap_systems
                    runtime_navigation3d_systems
                    runtime_physics3d_systems
                    runtime_presentation3d_systems
                    runtime_render_scene
                    runtime_render_backend_host
                    runtime_scene_script
                    runtime_frame
)
