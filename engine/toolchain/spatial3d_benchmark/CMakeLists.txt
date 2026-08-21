# Tool-only Spatial3D benchmark composition. The generator authors LXWA v4,
# cooks LXSC/LXES through the normal adapter and publishes ordinary Pak files.
if(NOT LUX_TARGET_OS STREQUAL "ANDROID")
    add_executable(
        lux_world_benchmark_generate
        ${CMAKE_CURRENT_SOURCE_DIR}/app/benchmark_generate.cpp
    )
    lux_classify_target(
        TARGET  lux_world_benchmark_generate
        LAYER   TOOLCHAIN
        PRODUCT TOOLCHAIN
        ROLE    COMPOSITION
    )
    target_link_libraries(
        lux_world_benchmark_generate
        PRIVATE
            lux::engine::toolchain::toolchain_spatial3d_scene_cook
            lux::engine::toolchain::toolchain_asset_cook
            lux::engine::authoring::authoring_world
            lux::engine::authoring::project
            lux::engine::ecs::core
            lux::engine::resource::asset
            lux::engine::resource::description
            lux::engine::content
            lux::engine::core::meta
    )
    if(TARGET ecs_meta AND
       TARGET render_components_meta AND
       TARGET render_components_3d_meta AND
       TARGET spatial3d_components_meta AND
       TARGET terrain_components_meta AND
       TARGET physics3d_meta AND
       TARGET navigation3d_meta)
        target_link_libraries(
            lux_world_benchmark_generate
            PRIVATE
                ecs_meta
                render_components_meta
                render_components_3d_meta
                spatial3d_components_meta
                terrain_components_meta
                physics3d_meta
                navigation3d_meta
        )
    endif()
    set_target_properties(
        lux_world_benchmark_generate
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    if(WIN32)
        add_custom_command(
            TARGET lux_world_benchmark_generate
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:lux_world_benchmark_generate>
                    $<TARGET_FILE_DIR:lux_world_benchmark_generate>
            COMMAND_EXPAND_LISTS
            COMMENT "Stage Spatial3D benchmark generator dependencies"
        )
    endif()

    if(WIN32 AND TARGET lux_world_benchmark)
        set(_lux_world_route_semantic_root
            "${CMAKE_BINARY_DIR}/p0-route-semantic-fixture")
        set(_lux_world_route_semantic_pak
            "${_lux_world_route_semantic_root}/Derived/windows-x64/WorldBenchmark.luxpak")
        set(_lux_world_route_semantic_artifacts
            "${CMAKE_BINARY_DIR}/p0-closeout/world-route-semantic")


    endif()
endif()
