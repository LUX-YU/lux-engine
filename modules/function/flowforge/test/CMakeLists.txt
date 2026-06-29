# lux::engine::function::flowforge — unit tests (enable with -DENABLE_FLOWFORGE_TEST=ON).
# Each test is a standalone self-checking executable returning exit code 0 on
# success, !=0 on failure (no GoogleTest dep).

# P0 of the GraphKit plan: dynamic-pin rebuild (rebind/reconstruct) +
# id-preserving restore (insertNodeAt / SparseSet try_emplace_at).
add_executable(flowforge_dynamism_test flowforge_dynamism_test.cpp)
target_link_libraries(flowforge_dynamism_test PRIVATE flowforge)