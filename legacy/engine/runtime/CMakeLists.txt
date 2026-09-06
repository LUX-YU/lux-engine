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
add_subdirectory(assets/physics3d)
add_subdirectory(scene/core)
add_subdirectory(scene/composition)
add_subdirectory(render/scene)
add_subdirectory(render/backend_host)
add_subdirectory(scene/script)
add_subdirectory(frame)

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
                    runtime_physics3d_prepare
                    runtime_scene_spatial3d_composition
                    runtime_scene_core
                    runtime_scene_tilemap_composition
                    runtime_scene_navigation3d_composition
                    runtime_scene_physics3d_composition
                    runtime_scene_presentation3d_composition
                    runtime_render_scene
                    runtime_render_backend_host
                    runtime_scene_script
                    runtime_frame
)
