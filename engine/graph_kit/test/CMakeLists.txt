# lux::engine::editor_graphkit — unit tests (enable with -DENABLE_GRAPHKIT_TEST=ON).
#
# Self-checking executable, exit 0 on success (no GoogleTest dep). Exercises
# the framework's PURE logic — command stack (inverse intents + id stability +
# cap-aware replace + transactions), fallback layout, PinIdBimap — against a
# stub IGraphView/IGraphSchema. Builds WITHOUT the editor component (the
# placement's enforcement payoff); the live canvas is exercised in P2 when the
# first real panel hosts a GraphEditor.

add_executable(graphkit_stub_test graphkit_stub_test.cpp)
target_link_libraries(graphkit_stub_test PRIVATE graph_kit)
# The bimap is a pinclude-private detail; the component's own test may see it.
target_include_directories(graphkit_stub_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude)
