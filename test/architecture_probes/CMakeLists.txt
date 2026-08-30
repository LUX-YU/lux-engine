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
