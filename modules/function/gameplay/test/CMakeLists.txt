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
