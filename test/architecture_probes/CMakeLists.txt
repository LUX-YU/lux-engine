foreach(probe IN ITEMS spatial3d spatial2d pixel robot)
    add_executable(architecture_probe_${probe} ${probe}_probe.cpp)
    target_link_libraries(
        architecture_probe_${probe}
        PRIVATE
            lux::engine::scene::scene_runtime_presentation
            lux::engine::simulation::ecs::transform
    )
    lux_classify_target(
        TARGET  architecture_probe_${probe}
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME architecture_probe_${probe} COMMAND architecture_probe_${probe})
    set_tests_properties(
        architecture_probe_${probe}
        PROPERTIES LABELS "architecture;probe;host"
    )
endforeach()

find_package(Jolt CONFIG REQUIRED)

add_executable(architecture_probe_jolt_l1 jolt_l1_probe.cpp)
target_link_libraries(
    architecture_probe_jolt_l1
    PRIVATE
        Jolt::Jolt
        lux::engine::simulation::simulation_core
        lux::engine::simulation::ecs::transform
)
lux_classify_target(
    TARGET  architecture_probe_jolt_l1
    LAYER   HOST
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(NAME architecture_probe_jolt_l1 COMMAND architecture_probe_jolt_l1)
set_tests_properties(
    architecture_probe_jolt_l1
    PROPERTIES LABELS "architecture;probe;host;physics3d;jolt"
)

add_executable(architecture_probe_render_lanes render_lane_probe.cpp)
target_include_directories(
    architecture_probe_render_lanes
    PRIVATE ${PROJECT_SOURCE_DIR}/modules/function/render/test
)
target_link_libraries(
    architecture_probe_render_lanes
    PRIVATE
        lux::engine::function::render_vulkan
        lux::engine::scene::scene_runtime_presentation
)
lux_classify_target(
    TARGET  architecture_probe_render_lanes
    LAYER   HOST
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(NAME architecture_probe_render_lanes COMMAND architecture_probe_render_lanes)
set_tests_properties(
    architecture_probe_render_lanes
    PROPERTIES
        LABELS "architecture;probe;host;presentation;render;gpu"
        SKIP_RETURN_CODE 77
        TIMEOUT 30
)

option(
    LUX_BUILD_PRELOADED_SPATIAL3D_PROBE
    "Build the gated World/Jolt/Render preloaded Spatial3D vertical slice"
    OFF
)
if(LUX_BUILD_PRELOADED_SPATIAL3D_PROBE)
    set(spatial3d_probe_fragment_spv
        ${CMAKE_CURRENT_BINARY_DIR}/preloaded_spatial3d.frag.spv
    )
    set(spatial3d_probe_gbuffer_fragment_spv
        ${CMAKE_CURRENT_BINARY_DIR}/preloaded_spatial3d_gbuffer.frag.spv
    )
    add_custom_command(
        OUTPUT ${spatial3d_probe_fragment_spv}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE}
            --target-env=vulkan1.2
            -fshader-stage=frag
            ${PROJECT_SOURCE_DIR}/modules/function/render/test/render_3d_semantic.frag
            -o ${spatial3d_probe_fragment_spv}
        DEPENDS ${PROJECT_SOURCE_DIR}/modules/function/render/test/render_3d_semantic.frag
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${spatial3d_probe_gbuffer_fragment_spv}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE}
            --target-env=vulkan1.2
            -fshader-stage=frag
            ${PROJECT_SOURCE_DIR}/modules/function/render/test/render_3d_semantic_gbuffer.frag
            -o ${spatial3d_probe_gbuffer_fragment_spv}
        DEPENDS ${PROJECT_SOURCE_DIR}/modules/function/render/test/render_3d_semantic_gbuffer.frag
        VERBATIM
    )
    add_custom_target(
        architecture_probe_spatial3d_preloaded_shaders
        DEPENDS ${spatial3d_probe_fragment_spv} ${spatial3d_probe_gbuffer_fragment_spv}
    )

    add_executable(architecture_probe_spatial3d_preloaded spatial3d_preloaded_probe.cpp)
    add_dependencies(
        architecture_probe_spatial3d_preloaded
        architecture_probe_spatial3d_preloaded_shaders
    )
    target_include_directories(
        architecture_probe_spatial3d_preloaded
        PRIVATE ${PROJECT_SOURCE_DIR}/modules/function/render/test
    )
    target_compile_definitions(
        architecture_probe_spatial3d_preloaded
        PRIVATE
            LUX_SPATIAL3D_FRAGMENT_SPV="${spatial3d_probe_fragment_spv}"
            LUX_SPATIAL3D_GBUFFER_FRAGMENT_SPV="${spatial3d_probe_gbuffer_fragment_spv}"
    )
    target_link_libraries(
        architecture_probe_spatial3d_preloaded
        PRIVATE
            Jolt::Jolt
            render_features
            lux::engine::function::render_vulkan
            lux::engine::scene::scene_core
            lux::engine::scene::scene_runtime_presentation
            lux::engine::scene::scene_runtime_world
            lux::engine::simulation::ecs::transform
    )
    lux_classify_target(
        TARGET  architecture_probe_spatial3d_preloaded
        LAYER   HOST
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME architecture_probe_spatial3d_preloaded COMMAND architecture_probe_spatial3d_preloaded)
    set_tests_properties(
        architecture_probe_spatial3d_preloaded
        PROPERTIES
            LABELS "architecture;probe;host;spatial3d;jolt;render;gpu"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
    )
endif()
