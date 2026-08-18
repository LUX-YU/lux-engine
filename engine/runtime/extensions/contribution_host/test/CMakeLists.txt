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

add_executable(
    render_effect_catalog_test
    render_effect_catalog_test.cpp
)

target_link_libraries(
    render_effect_catalog_test
    PRIVATE lux::engine::runtime::runtime_render_effects
)

lux_classify_target(
    TARGET  render_effect_catalog_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    DOMAIN
)
