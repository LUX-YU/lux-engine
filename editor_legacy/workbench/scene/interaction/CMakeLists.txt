add_component(COMPONENT_NAME scene_interaction_legacy NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/SceneInteraction.cpp)
component_include_directories(scene_interaction_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_interaction_legacy PUBLIC lux::engine::editor::scene_model_legacy lux::engine::editor::scene_execution_api_legacy)
component_add_transitive_commands(scene_interaction_legacy "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model_legacy)"
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_execution_api_legacy)")
lux_classify_target(TARGET scene_interaction_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-interaction VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_interaction_legacy)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_interaction_test_legacy test/interaction.cpp)
    target_link_libraries(editor_scene_interaction_test_legacy PRIVATE scene_interaction_legacy)
    lux_editor_test_options(editor_scene_interaction_test_legacy)
    add_test(NAME editor.scene_interaction_legacy COMMAND editor_scene_interaction_test_legacy)
    set_tests_properties(editor.scene_interaction_legacy PROPERTIES LABELS "editor;native;P08")
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_run_selection_test_legacy test/run_selection.cpp)
    target_link_libraries(editor_run_selection_test_legacy PRIVATE scene_interaction_legacy scene_execution_legacy lux::engine::scene::scene_transform)
    lux_editor_test_options(editor_run_selection_test_legacy)
    add_test(NAME editor.interaction_run_identity COMMAND editor_run_selection_test_legacy)
    set_tests_properties(editor.interaction_run_identity PROPERTIES LABELS "editor;native;P08")
endif()
