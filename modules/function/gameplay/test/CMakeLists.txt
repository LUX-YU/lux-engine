# TransformSystem hierarchy propagation (headless). Uses d3::installSystems on a
# neutral World.
add_executable(transform_system_test transform_system_test.cpp)
target_link_libraries(transform_system_test PRIVATE gameplay)
add_test(NAME transform_system_test COMMAND transform_system_test)

# Gate -1 / G-07: the TransformSystem OWNS the derived WorldTransform invariant —
# auto-emplaces it on any Transform entity (no loader/editor back-fill) and drops it
# when the source Transform is removed. Headless CPU.
add_executable(transform_auto_world_test transform_auto_world_test.cpp)
target_link_libraries(transform_auto_world_test PRIVATE gameplay)
add_test(NAME transform_auto_world_test COMMAND transform_auto_world_test)

# Gate -1 / G-08: hierarchy cycle detection (TransformSystem breaks + counts a parent
# cycle) + camera scale diagnostic (CameraSystem flags a scaled ancestry + de-scales
# the view). Headless CPU.
add_executable(hierarchy_cycle_test hierarchy_cycle_test.cpp)
target_link_libraries(hierarchy_cycle_test PRIVATE gameplay)
add_test(NAME hierarchy_cycle_test COMMAND hierarchy_cycle_test)

add_executable(camera_scale_test camera_scale_test.cpp)
target_link_libraries(camera_scale_test PRIVATE gameplay)
add_test(NAME camera_scale_test COMMAND camera_scale_test)

# WorldStreamingSystem cell gating (headless CPU, no GPU).
add_executable(world_streaming_test world_streaming_test.cpp)
target_link_libraries(world_streaming_test PRIVATE gameplay)
add_test(NAME world_streaming_test COMMAND world_streaming_test)

# Gate 0 / D-00: the 2D kit (lux::gameplay::d2) compiles + links, its D2ScenePlan
# builder works, and an empty plan installs no systems. Headless CPU.
add_executable(2d_empty_kit_compile_test 2d_empty_kit_compile_test.cpp)
target_link_libraries(2d_empty_kit_compile_test PRIVATE gameplay)
add_test(NAME 2d_empty_kit_compile_test COMMAND 2d_empty_kit_compile_test)

# Gate 0 / D-01: D2ScenePlan::validate() dependency rules + fixed-step bounds,
# covering the legal/illegal capability matrix. Headless CPU.
add_executable(d2_scene_plan_validation_test d2_scene_plan_validation_test.cpp)
target_link_libraries(d2_scene_plan_validation_test PRIVATE gameplay)
add_test(NAME d2_scene_plan_validation_test COMMAND d2_scene_plan_validation_test)

# Gate 0 / D-03: Simulation2DSystem fixed-step accumulator (clamp/cap/drop, physics-
# agnostic) + canonical phase order. Headless CPU.
add_executable(fixed_step_accumulator_test fixed_step_accumulator_test.cpp)
target_link_libraries(fixed_step_accumulator_test PRIVATE gameplay)
add_test(NAME fixed_step_accumulator_test COMMAND fixed_step_accumulator_test)

add_executable(simulation_phase_order_test simulation_phase_order_test.cpp)
target_link_libraries(simulation_phase_order_test PRIVATE gameplay)
add_test(NAME simulation_phase_order_test COMMAND simulation_phase_order_test)

# Gate 0 / D-02: d2::install() one-shot deterministic order — one Simulation2DSystem
# per fixed-step plan (single accumulator), refuse invalid/runtime-less plans. Headless.
add_executable(d2_install_order_test d2_install_order_test.cpp)
target_link_libraries(d2_install_order_test PRIVATE gameplay)
add_test(NAME d2_install_order_test COMMAND d2_install_order_test)

# Gate 0 / D-04: 2D scene lifecycle contract — reverse-order rollback of a
# half-initialised bring-up + reverse teardown, no dtor-sent commands. Headless.
add_executable(construction_failure_rollback_test construction_failure_rollback_test.cpp)
target_link_libraries(construction_failure_rollback_test PRIVATE gameplay)
add_test(NAME construction_failure_rollback_test COMMAND construction_failure_rollback_test)

# Slice A / T2-00+T2-01: Transform2DSystem — hierarchy composition + G-07 auto-maintain
# (via install), and negative/zero-scale + rotation-unit edge cases. Headless CPU.
add_executable(transform2d_hierarchy_test transform2d_hierarchy_test.cpp)
target_link_libraries(transform2d_hierarchy_test PRIVATE gameplay)
add_test(NAME transform2d_hierarchy_test COMMAND transform2d_hierarchy_test)

add_executable(negative_scale_test negative_scale_test.cpp)
target_link_libraries(negative_scale_test PRIVATE gameplay)
add_test(NAME negative_scale_test COMMAND negative_scale_test)

# Slice A / Camera2D: Camera2DSystem ortho view/proj from WorldTransform2D + zoom,
# auto-maintained Camera2DCache, wired by install(Core). Headless CPU.
add_executable(camera2d_projection_test camera2d_projection_test.cpp)
target_link_libraries(camera2d_projection_test PRIVATE gameplay)
add_test(NAME camera2d_projection_test COMMAND camera2d_projection_test)

# Slice A / T2-02+T2-03: Camera2D world<->screen round-trip helpers + explicit
# active-camera selection (never the implicit first). Headless CPU.
add_executable(round_trip_world_screen_test round_trip_world_screen_test.cpp)
target_link_libraries(round_trip_world_screen_test PRIVATE gameplay)
add_test(NAME round_trip_world_screen_test COMMAND round_trip_world_screen_test)

add_executable(active_camera_selection_test active_camera_selection_test.cpp)
target_link_libraries(active_camera_selection_test PRIVATE gameplay)
add_test(NAME active_camera_selection_test COMMAND active_camera_selection_test)

# Gate -1 / G-01: PoolBridge reap honours the trait's Require/Exclude view.
# Headless CPU (pure-ECS membership predicate; no RenderSession). Registered to
# CTest — the pre-existing two above get wired into CTest under G-09, alongside the
# fake-session fixture that unlocks the full bridge-dispatch regression.
add_executable(pool_bridge_reap_test pool_bridge_reap_test.cpp)
target_link_libraries(pool_bridge_reap_test PRIVATE gameplay)
add_test(NAME pool_bridge_reap_test COMMAND pool_bridge_reap_test)

# Gate -1 / G-09: PoolBridge full create→live→reap→teardown driven over a real
# RenderSession command channel against a fake, GPU-free in-process render server
# (HeadlessBridgeFixture: generic RenderServer<> + FrameDispatcher recording
# handlers). `gameplay` PUBLIC-links render + asset, so RenderSession /
# RenderServer<> / FeatureRegistry / AssetManager come in with no extra deps.
add_executable(pool_bridge_dispatch_reap_test pool_bridge_dispatch_reap_test.cpp)
target_link_libraries(pool_bridge_dispatch_reap_test PRIVATE gameplay)
add_test(NAME pool_bridge_dispatch_reap_test COMMAND pool_bridge_dispatch_reap_test)

# Gate -1 / G-09: RenderableSystem two-phase teardown over the same fake server —
# a LIVE light destroyed at beginShutdown (G-02), and a create still IN FLIGHT at
# shutdown drained into an orphan then destroyed by flushShutdownCleanup (P0 drain).
add_executable(renderable_shutdown_test renderable_shutdown_test.cpp)
target_link_libraries(renderable_shutdown_test PRIVATE gameplay)
add_test(NAME renderable_shutdown_test COMMAND renderable_shutdown_test)

# Gate -1 / G-09: InstanceBridge failed-create handling — an addMeshInstance reply
# with a non-Ok MeshInstanceCreateStatus must not go live (P1-2) and a permanent
# failure must not be re-issued every frame (G-05). Exercises the real ensureMesh
# upload path (a minimal in-memory MeshAsset) up to the create reply.
add_executable(instance_bridge_invalid_reply_test instance_bridge_invalid_reply_test.cpp)
target_link_libraries(instance_bridge_invalid_reply_test PRIVATE gameplay)
add_test(NAME instance_bridge_invalid_reply_test COMMAND instance_bridge_invalid_reply_test)

# Review round-5 / P1-3: a PoolBridge create whose reply is a generic dispatch-failure
# default {status 0, null handle} must not become a null-handle live zombie.
add_executable(pool_command_failed_test pool_command_failed_test.cpp)
target_link_libraries(pool_command_failed_test PRIVATE gameplay)
add_test(NAME pool_command_failed_test COMMAND pool_command_failed_test)

# Review round-5 / P1-4: an uploadMesh whose reply is {status 0, null handle} is marked
# failed (not a ready-but-null entry that re-uploads every frame).
add_executable(mesh_command_failed_test mesh_command_failed_test.cpp)
target_link_libraries(mesh_command_failed_test PRIVATE gameplay)
add_test(NAME mesh_command_failed_test COMMAND mesh_command_failed_test)

# Gate -1 / G-09: G-04 per-entry scene routing — each TransformWriteEntry carries its
# own scene_id (MeshStackProxy::updateTransform), so interleaved transform batches from
# different scenes never cross. Tests the proxy's client-side encoding directly.
add_executable(transform_batch_scene_routing_test transform_batch_scene_routing_test.cpp)
target_link_libraries(transform_batch_scene_routing_test PRIVATE gameplay)
add_test(NAME transform_batch_scene_routing_test COMMAND transform_batch_scene_routing_test)

# Gate -1 / G-06: ParamBridge teardown clear — a Skybox (a PARAM feature that binds a
# texture) must reset its server binding on beginShutdown (setEquirect null → disable),
# so a reused scene never samples a released texture (P1-1).
add_executable(skybox_param_clear_test skybox_param_clear_test.cpp)
target_link_libraries(skybox_param_clear_test PRIVATE gameplay)
add_test(NAME skybox_param_clear_test COMMAND skybox_param_clear_test)

# Slice A / S2-01: the full sprite chain — ECS SpriteComponent → Sprite2DBridge →
# Canvas2D submit op over the fake server (HeadlessBridgeFixture). Verifies the ECS →
# SpriteDraw mapping (transform / size / tint / DrawOrderKey) headlessly.
add_executable(sprite_slice_a_test sprite_slice_a_test.cpp)
target_link_libraries(sprite_slice_a_test PRIVATE gameplay)
add_test(NAME sprite_slice_a_test COMMAND sprite_slice_a_test)
