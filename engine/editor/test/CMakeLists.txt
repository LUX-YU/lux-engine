## engine/editor/test — opt-in smoke tests for the editor library.
##
## Gated by ENABLE_EDITOR_TEST (set in engine/editor/CMakeLists.txt) so a
## default build doesn't compile or link them. Each test is a standalone
## executable runnable from the build tree.

# ── Phase B smoke: Project + ProjectManifest round-trip ─────────────────
add_executable(
    project_roundtrip_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/project_roundtrip_smoke.cpp
)
target_link_libraries(
    project_roundtrip_smoke
    PRIVATE
        editor
)

# ── Phase D smoke: Scene save / load round-trip ─────────────────────────
add_executable(
    scene_roundtrip_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/scene_roundtrip_smoke.cpp
)
target_link_libraries(
    scene_roundtrip_smoke
    PRIVATE
        editor
        lux::engine::function::ecs
        lux::engine::function::render_bridge
        pack_d3
)

# Force-include the gameplay meta sidecar so the reflection + component-type
# registries are populated when the test runs (all component reflection — neutral
# core + 3D — lives in gameplay_meta again).
if(TARGET ecs_meta)
    target_link_libraries(scene_roundtrip_smoke PRIVATE ecs_meta)
endif()
if(TARGET pack_d3_meta)
    target_link_libraries(scene_roundtrip_smoke PRIVATE pack_d3_meta)
endif()

# ── big_demo_baker: generate the large multi-light streaming demo scene ──
add_executable(
    big_demo_baker
    ${CMAKE_CURRENT_SOURCE_DIR}/big_demo_baker.cpp
)
target_link_libraries(
    big_demo_baker
    PRIVATE
        editor
        lux::engine::function::ecs
        lux::engine::function::render_bridge
        pack_d3
)
# Force-load the gameplay meta sidecar so the component reflection registry is
# populated before Scene::save runs.
if(TARGET ecs_meta)
    target_link_libraries(big_demo_baker PRIVATE ecs_meta)
endif()
if(TARGET pack_d3_meta)
    target_link_libraries(big_demo_baker PRIVATE pack_d3_meta)
endif()

# ── Thumbnail storage smoke: PNG codec + ThumbnailSet + payload round-trip ──
add_executable(
    thumbnail_payload_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/thumbnail_payload_smoke.cpp
)
target_link_libraries(
    thumbnail_payload_smoke
    PRIVATE
        editor
)

# ── Thumbnail GPU test: PreviewScene + renderers for texture/mesh/model/material ──
add_executable(
    thumbnail_render_gpu_test
    ${CMAKE_CURRENT_SOURCE_DIR}/thumbnail_render_gpu_test.cpp
)
target_link_libraries(
    thumbnail_render_gpu_test
    PRIVATE
        editor
        lux::engine::function::render
        lux::engine::platform::window
        lux::engine::resource::asset
)
