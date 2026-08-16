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
            lux::engine::resource::asset_codecs
            lux::engine::resource::description
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

        add_test(
            NAME world_route_semantic_fixture_generate
            COMMAND lux_world_benchmark_generate
                --output=${_lux_world_route_semantic_root}
                --scale=smoke
                --platform=windows-x64
                --cc0-instance-percent=0
        )
        set_tests_properties(
            world_route_semantic_fixture_generate
            PROPERTIES
                LABELS "cpu;toolchain;entity_scene;spatial3d;semantic"
                FIXTURES_SETUP world_route_semantic_fixture
                TIMEOUT 180
        )

        add_test(
            NAME world_route_semantic_gpu_probe
            COMMAND lux_world_benchmark
                --pak=${_lux_world_route_semantic_pak}
                --scene=Scenes/Benchmark
                --world-edge=4096
                --repeat=1
                --warmup=1
                --frames=1
                --size=640x360
                --vk-validation
                --semantic-capture-dir=${_lux_world_route_semantic_artifacts}
                --require-route-semantics
                --json=${_lux_world_route_semantic_artifacts}/benchmark.json
                --csv=${_lux_world_route_semantic_artifacts}/frames.csv
        )
        set_tests_properties(
            world_route_semantic_gpu_probe
            PROPERTIES
                LABELS "gpu;entity_scene;spatial3d;semantic"
                FIXTURES_REQUIRED world_route_semantic_fixture
                TIMEOUT 360
        )
    endif()
endif()
