## Editor executable composition root. The editor domain library remains in
## engine/editor; only service construction, reflection sidecar selection and
## main() live here.

add_executable(lux_editor ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp)

lux_classify_target(
    TARGET  lux_editor
    LAYER   HOST
    PRODUCT EDITOR
    ROLE    COMPOSITION
)

target_link_libraries(
    lux_editor
    PRIVATE
        editor
        lux::engine::ecs::animation_3d
        lux::engine::ecs::physics
        lux::engine::ecs::presentation_2d
        lux::engine::ecs::transform
        lux::engine::runtime::runtime_tilemap_systems
        lux::engine::runtime::runtime_tilemap_prepare
        lux::engine::runtime::runtime_navigation3d_systems
        lux::engine::runtime::runtime_physics3d_systems
        lux::engine::runtime::runtime_presentation3d_systems
        lux::engine::runtime::runtime_spatial3d_streaming_systems
        lux::engine::ecs::scripting
        lux::engine::ecs::scripting_lua
        lux::engine::ecs::scripting_native
)

target_include_directories(
    lux_editor
    PRIVATE ${CMAKE_SOURCE_DIR}/engine/hosts/launcher/include
)

if(TARGET ecs_meta)
    target_link_libraries(lux_editor PRIVATE ecs_meta)
endif()
foreach(_sidecar
        render_components_meta
        render_components_2d_meta
        render_components_3d_meta
        spatial2d_components_meta
        spatial3d_components_meta
        terrain_components_meta
        tilemap_meta
        pixel_meta
        animation_2d_meta
        physics2d_meta
        physics3d_meta
        navigation3d_meta
        script_meta
        render_meta)
    if(TARGET ${_sidecar})
        target_link_libraries(lux_editor PRIVATE ${_sidecar})
    endif()
endforeach()
foreach(_lua_sidecar
        ecs_lua_meta
        render_components_lua_meta
        render_components_2d_lua_meta
        render_components_3d_lua_meta
        pixel_lua_meta
        animation_2d_lua_meta
        script_lua_meta)
    if(TARGET ${_lua_sidecar})
        target_link_libraries(lux_editor PRIVATE ${_lua_sidecar})
    endif()
endforeach()

if(TARGET nfd)
    target_link_libraries(lux_editor PRIVATE nfd)
endif()

set_target_properties(
    lux_editor
    PROPERTIES OUTPUT_NAME "lux_editor"
)

add_dependencies(lux_editor engine_content_bake)
install(
    TARGETS lux_editor
    RUNTIME DESTINATION bin
    COMPONENT lux_editor
)
