add_executable(
    lux_world_benchmark
    ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp
)

lux_classify_target(
    TARGET  lux_world_benchmark
    LAYER   HOST
    PRODUCT TEST
    ROLE    COMPOSITION
)

target_link_libraries(
    lux_world_benchmark
    PRIVATE
        lux::cxx::core
        lux::engine::host::game_application
        lux::engine::platform::window
        lux::engine::core::log
)

# Runtime component reflection follows the same explicit sidecar contract as
# lux_player. A benchmark without it would successfully open LXES while
# materializing no typed components.
foreach(_sidecar
        ecs_meta
        render_components_meta
        render_components_2d_meta
        render_components_3d_meta
        spatial3d_components_meta
        terrain_components_meta
        tilemap_meta
        pixel_meta
        animation_2d_meta
        physics2d_meta
        physics3d_meta
        navigation3d_meta
        script_meta)
    if(TARGET ${_sidecar})
        target_link_libraries(lux_world_benchmark PRIVATE ${_sidecar})
    endif()
endforeach()

set_target_properties(
    lux_world_benchmark
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_custom_command(
    TARGET lux_world_benchmark
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:lux_world_benchmark>
            $<TARGET_FILE_DIR:lux_world_benchmark>
    COMMAND_EXPAND_LISTS
    COMMENT "Stage lux_world_benchmark runtime dependency closure"
)
