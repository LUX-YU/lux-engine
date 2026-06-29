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
