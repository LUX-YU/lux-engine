add_executable(node_graph_editor_test graphkit_stub_test.cpp)
target_link_libraries(node_graph_editor_test PRIVATE node_graph_editor)
target_link_libraries(
    node_graph_editor_test
    PRIVATE
        lux::engine::function::flowforge
        lux::engine::function::material_graph
)
lux_classify_target(
    TARGET  node_graph_editor_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME node_graph_editor COMMAND node_graph_editor_test)
