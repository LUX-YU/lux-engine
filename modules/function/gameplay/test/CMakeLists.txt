# TransformSystem hierarchy propagation (headless). Uses d3::installSystems on a
# neutral World.
add_executable(transform_system_test transform_system_test.cpp)
target_link_libraries(transform_system_test PRIVATE gameplay)

# WorldStreamingSystem cell gating (headless CPU, no GPU).
add_executable(world_streaming_test world_streaming_test.cpp)
target_link_libraries(world_streaming_test PRIVATE gameplay)

# Gate -1 / G-01: PoolBridge reap honours the trait's Require/Exclude view.
# Headless CPU (pure-ECS membership predicate; no RenderSession). Registered to
# CTest — the pre-existing two above get wired into CTest under G-09, alongside the
# fake-session fixture that unlocks the full bridge-dispatch regression.
add_executable(pool_bridge_reap_test pool_bridge_reap_test.cpp)
target_link_libraries(pool_bridge_reap_test PRIVATE gameplay)
add_test(NAME pool_bridge_reap_test COMMAND pool_bridge_reap_test)
