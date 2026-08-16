## Runtime product layer.  Subdirectories are ordered by dependency, not by
## deployment unit: source boundaries remain explicit even when components are
## linked statically into a host.

add_subdirectory(execution)
add_subdirectory(assets)
add_subdirectory(extensions)
add_subdirectory(logging)
add_subdirectory(entity_scene)
add_subdirectory(spatial_partition)
add_subdirectory(spatial2d/tilemap)
add_subdirectory(spatial2d/infinite)
add_subdirectory(spatial3d/transform)
add_subdirectory(spatial3d/partitioned)
add_subdirectory(spatial3d/physics)
add_subdirectory(spatial3d/navigation)
add_subdirectory(scene/core)
add_subdirectory(packs)
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
                    runtime_scene_contributions
                    runtime_render_effects
                    runtime_contribution_host
                    runtime_logging
                    runtime_entity_scene
                    runtime_spatial_partition
                    runtime_spatial2d_tilemap
                    runtime_spatial2d_infinite
                    runtime_pack_infinite2d
                    runtime_pack_spatial3d_transform
                    runtime_spatial3d_partitioned
                    runtime_pack_spatial3d_partitioned
                    runtime_spatial3d_physics
                    runtime_spatial3d_navigation_adapter
                    runtime_scene_core
                    runtime_pack_physics2d
                    runtime_pack_presentation2d
                    runtime_pack_simulation2d
                    runtime_pack_spatial2d_transform
                    runtime_pack_tilemap2d
                    runtime_pack_animation3d
                    runtime_pack_navigation3d
                    runtime_pack_physics3d
                    runtime_pack_presentation3d
                    runtime_render_scene
                    runtime_render_backend_host
                    runtime_scene_script
                    runtime_frame
)
