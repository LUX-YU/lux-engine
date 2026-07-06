set(SHADER_COMPILE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/gen/shaders)
set(SHADER_CONFIG_OUT_PATH     ${CMAKE_BINARY_DIR}/gen/test/function/render/include)
set(RENDER_PINCLUDE_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude)

find_package(imgui REQUIRED COMPONENTS glfw3 vulkan)

component_generate_runtime_asset_path_header(
    COMPONENT render
    OUTPUT_HEADER ${SHADER_CONFIG_OUT_PATH}/test_time_asset_path.hpp
)

# game_loop_test -> RELOCATED to engine/shadergen_glsl/test (W5a: ported to a
# graph material; needs the engine-tier cook backend, which a modules/ test can't link).

# T2b — GPU->CPU offscreen-view color readback experiment (self-checking).
add_executable(
	readback_test
	readback_test.cpp
)

target_link_libraries(
	readback_test
	PRIVATE
	render
	lux::engine::platform::window
	lux::engine::resource::asset
)

target_include_directories(
	readback_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# thumbnail_render_test -> DELETED (W5a). Its only subject (offscreen render +
# readbackView) is covered by the editor's graph-material thumbnail_render_gpu_test
# (MATERIAL case) and the cleared-view readback by readback_test; its builtin
# grey material was pure scaffolding. No unique coverage lost.

# Plan A acceptance gate — two coexisting scenes (opposite cast-shadow lights)
# must render in isolation: scene A unchanged whether or not scene B exists.
# Fails on pre-Plan-A (global Light/Shadow singletons); passes post-Plan-A.
# two_scene_shadow_isolation_test -> RELOCATED to engine/shadergen_glsl/test (W5a).

# Phase D — deferred multi-shading-model dispatch proof (self-checking). Three
# spheres, identical grey albedo, PBR/Unlit/Toon models -> distinct output.
# deferred_multi_shading_model_test -> RELOCATED to engine/shadergen_glsl/test
# (W5a: the three SM materials are graphs now; needs the cook backend).

# SceneDescriptorArena unit test — headless Vulkan device; verifies the
# per-scene growable descriptor-pool chain grows on exhaustion, tears down
# cleanly, and throws (no infinite grow) when the template is too small.
add_executable(
	descriptor_arena_growth_test
	descriptor_arena_growth_test.cpp
)

target_link_libraries(
	descriptor_arena_growth_test
	PRIVATE
	render
	lux::engine::platform::window
)

target_include_directories(
	descriptor_arena_growth_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# ShaderResources SPIR-V dedup + refcount unit test — headless Vulkan device;
# proves identical SPIR-V collapses onto one shared VkShaderModule (the
# Material/Instance "share one PSO" foundation) and that the shared module is
# refcounted (last release destroys; cache evicts so a re-add mints afresh).
add_executable(
	shader_dedup_test
	shader_dedup_test.cpp
)

target_link_libraries(
	shader_dedup_test
	PRIVATE
	render
	lux::engine::platform::window
)

target_include_directories(
	shader_dedup_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# VariantBucketManager graph render-state keying unit test (W3a) — device-free;
# proves distinct render-state (alpha_mode / double_sided) -> distinct graph
# bucket (=distinct PSO), identical -> shared, and the bucket carries the state
# (DOUBLE_SIDED feature -> pick() no-cull tier).
add_executable(
	graph_bucket_render_state_test
	graph_bucket_render_state_test.cpp
)

target_link_libraries(
	graph_bucket_render_state_test
	PRIVATE
	render
	lux::engine::platform::window
)

target_include_directories(
	graph_bucket_render_state_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# skeletal_mesh_walk_test -> RELOCATED to engine/shadergen_glsl/test (W5a: ported
# to a graph material; needs the cook backend. The skinning subject is unchanged.)

# skeletal_world_demo -> DISABLED (W5c). This rigged-model viewer is the last test
# still authoring closure rdesc::Material (a flat PBR floor + a kSolidColorMaterial
# override) + lux::asset::MaterialAsset — both deleted by W5. Unlike its siblings
# (skeletal_mesh_walk / deferred_stress / game_loop), it was NOT relocated to
# engine/shadergen_glsl/test, so it can't reach the graph cook backend to bake
# a graph material. Its skinning + animated-shadow coverage is already provided by
# the relocated skeletal_mesh_walk_test, so it is dropped from the build here pending
# a proper relocation+port to graph materials (tracked as a follow-up). The source
# is kept in-tree for that port.
#
# add_executable(skeletal_world_demo skeletal_world_demo.cpp)
# target_link_libraries(skeletal_world_demo PRIVATE render gameplay animation
#     lux::engine::platform::window lux::engine::resource::asset)
# target_include_directories(skeletal_world_demo PRIVATE
#     ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

add_executable(
	point_cloud_stress_test
	point_cloud_stress_test.cpp
)

target_link_libraries(
	point_cloud_stress_test
	PRIVATE
	render
	lux::engine::platform::window
	lux::engine::resource::asset
)

target_include_directories(
	point_cloud_stress_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# deferred_stress_test -> RELOCATED to engine/shadergen_glsl/test (W5a: 3 graph
# materials now; needs the cook backend).

add_executable(
	point_cloud_transient_test
	point_cloud_transient_test.cpp
)

target_link_libraries(
	point_cloud_transient_test
	PRIVATE
	render
	lux::engine::platform::window
	lux::engine::resource::asset
)

target_include_directories(
	point_cloud_transient_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

add_executable(
	trajectory_line_test
	trajectory_line_test.cpp
)

target_link_libraries(
	trajectory_line_test
	PRIVATE
	render
	lux::engine::platform::window
	lux::engine::resource::asset
)

target_include_directories(
	trajectory_line_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

add_executable(
	gizmo_transient_test
	gizmo_transient_test.cpp
)

target_link_libraries(
	gizmo_transient_test
	PRIVATE
	render
	lux::engine::platform::window
	lux::engine::resource::asset
)

target_include_directories(
	gizmo_transient_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

add_executable(
	render_graph_loadop_policy_test
	render_graph_loadop_policy_test.cpp
)

target_link_libraries(
	render_graph_loadop_policy_test
	PRIVATE
	render
)

target_include_directories(
	render_graph_loadop_policy_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
)

add_executable(
	render_graph_qfot_guard_test
	render_graph_qfot_guard_test.cpp
)

target_link_libraries(
	render_graph_qfot_guard_test
	PRIVATE
	render
)

target_include_directories(
	render_graph_qfot_guard_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
)

add_executable(
	submission_merge_test
	submission_merge_test.cpp
)

target_link_libraries(
	submission_merge_test
	PRIVATE
	render
)

target_include_directories(
	submission_merge_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
)

add_executable(
	shadow_slice_math_test
	shadow_slice_math_test.cpp
)

target_link_libraries(
	shadow_slice_math_test
	PRIVATE
	render
)

target_include_directories(
	shadow_slice_math_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
)

# ping_pong_test — CPU-only check of the RG ping-pong resource model (M1):
# createPingPong handle/descriptor layout + resolveRingTarget redirect math.
add_executable(
	ping_pong_test
	ping_pong_test.cpp
)

target_link_libraries(
	ping_pong_test
	PRIVATE
	render
)

target_include_directories(
	ping_pong_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
)

# ping_pong_rg_test — M2: ping-pong cross-feature subscription (referencePingPong) +
# required/optional dependency contract + compiled-graph minimal-overhead dump.
add_executable(
	ping_pong_rg_test
	ping_pong_rg_test.cpp
)

target_link_libraries(
	ping_pong_rg_test
	PRIVATE
	render
)

target_include_directories(
	ping_pong_rg_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
)

# P2 HZB Stage A — Hi-Z (max-Z) pyramid build numerical self-check. Headless
# device; uploads a known depth, builds the pyramid via HzbResources::recordBuild,
# reads every mip back and asserts each texel == the CPU max-Z reference.
add_executable(
	hzb_pyramid_test
	hzb_pyramid_test.cpp
)

target_link_libraries(
	hzb_pyramid_test
	PRIVATE
	render
	lux::engine::platform::window
)

target_include_directories(
	hzb_pyramid_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# spatial_cull_grid_test — CPU-only check of SpatialCullGrid's pure cell math (W1):
# cellCoord (world XY -> integer cell, floor incl. negatives) + cellInRange (2D
# distance vs cull_distance + one-cell slack; Z ignored; empty cameras -> active).
add_executable(
    spatial_cull_grid_test
    spatial_cull_grid_test.cpp
)

target_link_libraries(
    spatial_cull_grid_test
    PRIVATE
    render
)

target_include_directories(
    spatial_cull_grid_test
    PRIVATE
    ${RENDER_PINCLUDE_DIR}
)

# spatial_cull_grid_stress_test — headless GPU numerical proof of the W1 coarse-cull
# payoff: a ~25k-instance grid, camera recedes -> activeInstanceCount is
# non-increasing and falls to 0; read back the GPU active-mask buffer and assert
# dormant=0 / under-camera=1 / 1-count == activeInstanceCount. Uses the
# InstanceResources-free SpatialCullGrid::update overload (no full instance storage).
add_executable(
    spatial_cull_grid_stress_test
    spatial_cull_grid_stress_test.cpp
)

target_link_libraries(
    spatial_cull_grid_stress_test
    PRIVATE
    render
    lux::engine::platform::window
)

target_include_directories(
    spatial_cull_grid_stress_test
    PRIVATE
    ${RENDER_PINCLUDE_DIR}
    ${SHADER_CONFIG_OUT_PATH}
)

# feature_view_lifecycle_test — asserts the feature/view lifecycle matrix (PR-1),
# the FeatureState enable/disable view-state management (3b), and dependency
# hard-reject (3c), via a test CountingFeature driven through the server+session.
add_executable(
	feature_view_lifecycle_test
	feature_view_lifecycle_test.cpp
)

target_link_libraries(
	feature_view_lifecycle_test
	PRIVATE
	render
	lux::engine::platform::window
)

target_include_directories(
	feature_view_lifecycle_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# exportable_memory_test — asserts the domain-neutral CUDA<->Vulkan external-memory
# seam (createExportableBuffer / createExportableTimelineSemaphore + raw-handle
# retire) via a neutral ExportProbeFeature. No CUDA, no downstream/Gaussian code.
add_executable(
	exportable_memory_test
	exportable_memory_test.cpp
)

target_link_libraries(
	exportable_memory_test
	PRIVATE
	render
	lux::engine::platform::window
)

target_include_directories(
	exportable_memory_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# R2-00 — Canvas2D public protocol freeze. Both are pure header-only POD checks
# (no Vulkan device); they only need render's PUBLIC include dir, reachable by linking
# `render`. Kept device-free so they ride the `cpu` tier.
add_executable(protocol_static_asserts protocol_static_asserts.cpp)
target_link_libraries(protocol_static_asserts PRIVATE render)

add_executable(draw_order_comparator_test draw_order_comparator_test.cpp)
target_link_libraries(draw_order_comparator_test PRIVATE render)

# R2-01 — Canvas2DFeature factory + descriptor + submit-op protocol (device-free half).
add_executable(canvas2d_factory_static_test canvas2d_factory_static_test.cpp)
target_link_libraries(canvas2d_factory_static_test PRIVATE render)

# R2-03 — painter-order sort (pure logic, device-free).
add_executable(cross_producer_order_test cross_producer_order_test.cpp)
target_link_libraries(cross_producer_order_test PRIVATE render)

# R2-04 — Canvas2D op-ids are addressable by name; absent → invalid (3D scene no-ops).
add_executable(feature_absent_noop_test feature_absent_noop_test.cpp)
target_link_libraries(feature_absent_noop_test PRIVATE render)

add_executable(feature_present_ops_test feature_present_ops_test.cpp)
target_link_libraries(feature_present_ops_test PRIVATE render)

# R2-01 — Canvas2DFeature attach/detach lifecycle over the server+session harness
# (needs a device; skips cleanly if no Vulkan). Mirrors feature_view_lifecycle_test.
add_executable(canvas2d_feature_lifecycle_test canvas2d_feature_lifecycle_test.cpp)
target_link_libraries(
	canvas2d_feature_lifecycle_test
	PRIVATE
	render
	lux::engine::platform::window
)
target_include_directories(
	canvas2d_feature_lifecycle_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# R2-02 — the Canvas2D render graph is stable under per-frame content change
# (submitting sprites causes no recompile). Needs a device; skips if no Vulkan.
add_executable(graph_dump_stability_test graph_dump_stability_test.cpp)
target_link_libraries(
	graph_dump_stability_test
	PRIVATE
	render
	lux::engine::platform::window
)
target_include_directories(
	graph_dump_stability_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# S2-03 — the Sprite Slice A visual gate: Canvas2D sprites render to the RIGHT
# pixels on a real device (offscreen render + readback). Uses DeviceRenderFixture.hpp.
add_executable(sprite2d_offscreen_test sprite2d_offscreen_test.cpp)
target_link_libraries(
	sprite2d_offscreen_test
	PRIVATE
	render
	lux::engine::platform::window
)
target_include_directories(
	sprite2d_offscreen_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# S2-03 visual — the same sprites, but ON SCREEN (swapchain-bound, loops until closed).
# Interactive: for eyeballing the 2D pipeline, not CI. Uses DeviceRenderFixture.hpp.
add_executable(sprite2d_visual_demo sprite2d_visual_demo.cpp)
target_link_libraries(
	sprite2d_visual_demo
	PRIVATE
	render
	lux::engine::platform::window
)
target_include_directories(
	sprite2d_visual_demo
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# ─────────────────────────────────────────────────────────────────────────────
#  CTest registration (render 重构阶段 0 — 测试基线)
#
#  Three capability labels so CI can select by what a runner actually provides:
#    cpu          — device-free; self-checking via process exit code. Safe on
#                   any runner (some still LINK platform::window for a test
#                   helper, but create no Vulkan device at run time).
#    gpu          — headless Vulkan; self-checking; terminate on their own.
#                   Need a GPU runner (or a software ICD: Lavapipe / SwiftShader).
#    interactive  — open a window and loop until closed (while(!shouldClose())).
#                   NOT for unattended CI; the TIMEOUT below turns an accidental
#                   headless run into a fail-fast instead of a hang.
#
#  Typical CI:  ctest -L cpu             # no GPU needed
#               ctest -LE interactive    # GPU runner: cpu + gpu, skip windowed
#
#  Follow-up (tracked in .internal/UNFINISHED-WORK.md): give the interactive
#  tests a headless/frame-capped self-exit mode so they can join the `gpu` tier;
#  optionally split CPU-only tests off imgui/window so a `cpu` build needs no
#  graphics deps. Out of scope for the baseline PR.
# ─────────────────────────────────────────────────────────────────────────────

function(lux_render_add_test target)
    cmake_parse_arguments(ARG "" "LABEL;TIMEOUT" "" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        LABELS   "${ARG_LABEL}"
        TIMEOUT  ${ARG_TIMEOUT})
endfunction()

# CPU-only (device-free) — always CI-safe
lux_render_add_test(spatial_cull_grid_test          LABEL cpu)
lux_render_add_test(ping_pong_test                  LABEL cpu)
lux_render_add_test(ping_pong_rg_test               LABEL cpu)
lux_render_add_test(render_graph_loadop_policy_test LABEL cpu)
lux_render_add_test(render_graph_qfot_guard_test    LABEL cpu)
lux_render_add_test(submission_merge_test           LABEL cpu)
lux_render_add_test(shadow_slice_math_test          LABEL cpu)
lux_render_add_test(graph_bucket_render_state_test  LABEL cpu)
lux_render_add_test(protocol_static_asserts         LABEL cpu)
lux_render_add_test(draw_order_comparator_test      LABEL cpu)
lux_render_add_test(canvas2d_factory_static_test    LABEL cpu)
lux_render_add_test(cross_producer_order_test       LABEL cpu)
lux_render_add_test(feature_absent_noop_test        LABEL cpu)
lux_render_add_test(feature_present_ops_test        LABEL cpu)

# Headless Vulkan, self-checking, self-terminating — need a GPU runner
lux_render_add_test(descriptor_arena_growth_test    LABEL gpu)
lux_render_add_test(shader_dedup_test               LABEL gpu)
lux_render_add_test(readback_test                   LABEL gpu)
lux_render_add_test(feature_view_lifecycle_test      LABEL gpu)
lux_render_add_test(canvas2d_feature_lifecycle_test  LABEL gpu)
lux_render_add_test(graph_dump_stability_test        LABEL gpu)
lux_render_add_test(sprite2d_offscreen_test          LABEL gpu)
lux_render_add_test(exportable_memory_test          LABEL gpu)
lux_render_add_test(hzb_pyramid_test                LABEL gpu TIMEOUT 300)
lux_render_add_test(spatial_cull_grid_stress_test   LABEL gpu TIMEOUT 300)

# Interactive (open a window, loop until closed) — NOT for unattended CI
lux_render_add_test(point_cloud_stress_test         LABEL interactive)
lux_render_add_test(point_cloud_transient_test      LABEL interactive)
lux_render_add_test(trajectory_line_test            LABEL interactive)
lux_render_add_test(gizmo_transient_test            LABEL interactive)
lux_render_add_test(sprite2d_visual_demo            LABEL interactive)
