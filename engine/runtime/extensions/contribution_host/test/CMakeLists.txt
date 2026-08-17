add_executable(
    scene_contributions_test
    scene_contributions_test.cpp
)

target_link_libraries(
    scene_contributions_test
    PRIVATE lux::engine::runtime::runtime_scene_contributions
)

lux_classify_target(
    TARGET  scene_contributions_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    DOMAIN
)
