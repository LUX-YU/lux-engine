add_component(COMPONENT_NAME scene_interaction NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/SceneInteraction.cpp)
component_include_directories(scene_interaction BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_interaction PUBLIC lux::engine::editor::scene_model lux::engine::editor::scene_execution_api)
component_add_transitive_commands(scene_interaction "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model)"
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_execution_api)")
lux_classify_target(TARGET scene_interaction LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-interaction VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_interaction)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_interaction_test test/interaction.cpp)
    target_link_libraries(editor_scene_interaction_test PRIVATE scene_interaction)
    lux_editor_test_options(editor_scene_interaction_test)
    add_test(NAME editor.scene_interaction COMMAND editor_scene_interaction_test)
    set_tests_properties(editor.scene_interaction PROPERTIES LABELS "editor;native;P08")
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_run_selection_test test/run_selection.cpp)
    target_link_libraries(editor_run_selection_test PRIVATE scene_interaction scene_execution lux::engine::scene::scene_transform)
    lux_editor_test_options(editor_run_selection_test)
    add_test(NAME editor.interaction_run_identity COMMAND editor_run_selection_test)
    set_tests_properties(editor.interaction_run_identity PROPERTIES LABELS "editor;native;P08")
endif()
