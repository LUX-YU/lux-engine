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
