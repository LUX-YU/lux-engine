add_executable(editor_edit_history_routing_test EditHistoryRoutingTest.cpp)
target_link_libraries(editor_edit_history_routing_test PRIVATE editor_context)
target_include_directories(editor_edit_history_routing_test PRIVATE ../../editing/test ../../editing/pinclude)
if(MSVC)
    target_compile_options(editor_edit_history_routing_test PRIVATE /UNDEBUG)
else()
    target_compile_options(editor_edit_history_routing_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET editor_edit_history_routing_test LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
add_test(NAME editor_edit_history_routing COMMAND editor_edit_history_routing_test)
set_tests_properties(editor_edit_history_routing PROPERTIES LABELS "editor;editing;ed1;routing;object;ui")
add_library(
    editor_context_plugin_test_support
    STATIC
    PluginProbe.cpp
)
target_link_libraries(
    editor_context_plugin_test_support
    PUBLIC lux::engine::editor::editor_context
)
lux_classify_target(
    TARGET  editor_context_plugin_test_support
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    FOUNDATION
)

add_executable(editor_context_test editor_context_test.cpp)
target_include_directories(
    editor_context_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude
        ${PROJECT_SOURCE_DIR}/engine/editor/application/tooling/pinclude
)
target_link_libraries(
    editor_context_test
    PRIVATE
        lux::engine::editor::editor_context
        editor_context_plugin_test_support
        lux::engine::scene::scene_composition
        lux::engine::scene::scene_description
        lux::engine::simulation::simulation_description
        lux::engine::world::world_description
)
if(MSVC)
    target_compile_options(editor_context_test PRIVATE /UNDEBUG)
else()
    target_compile_options(editor_context_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  editor_context_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME editor_context COMMAND editor_context_test)


set_tests_properties(editor_context PROPERTIES LABELS "editor;editing;ed1;regression;context")
