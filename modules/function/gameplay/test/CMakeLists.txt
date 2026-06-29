# TransformSystem hierarchy propagation (headless). Uses d3::installSystems on a
# neutral World.
add_executable(transform_system_test transform_system_test.cpp)
target_link_libraries(transform_system_test PRIVATE gameplay)

# WorldStreamingSystem cell gating (headless CPU, no GPU).
add_executable(world_streaming_test world_streaming_test.cpp)
target_link_libraries(world_streaming_test PRIVATE gameplay)
