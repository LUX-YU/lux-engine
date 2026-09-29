add_subdirectory(api)

add_component(COMPONENT_NAME scene_execution NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/RunStore.cpp)
component_include_directories(scene_execution
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_execution PUBLIC lux::engine::editor::scene_execution_api
    lux::engine::editor::editor_editing_scene lux::engine::scene::scene_render
    lux::engine::scene::scene_world_loading lux::engine::process::world_loading)
component_add_transitive_commands(scene_execution
    "find_package(lux-engine-editor-editing-scene REQUIRED COMPONENTS editor_editing_scene)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)")
lux_classify_target(TARGET scene_execution LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-execution VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_execution_api scene_execution)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_execution_test test/runs.cpp)
    target_link_libraries(editor_scene_execution_test PRIVATE scene_execution lux::engine::scene::scene_transform)
    lux_editor_test_options(editor_scene_execution_test)
    foreach(scenario isolation controls failure completion r1-queued r1-mixed r1-failed r1-callback r1-capacity)
        add_test(NAME editor.scene_execution.${scenario} COMMAND editor_scene_execution_test ${scenario})
        set_tests_properties(editor.scene_execution.${scenario} PROPERTIES LABELS "editor;native;P06")
    endforeach()
endif()
