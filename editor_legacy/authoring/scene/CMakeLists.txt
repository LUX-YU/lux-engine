add_component(COMPONENT_NAME scene_model_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneConfigurationDraft.cpp src/AuthoringFacts.cpp src/PreparedSceneReload.cpp src/SceneSnapshot.cpp src/SceneSource.cpp src/SceneSession.cpp src/ScenePersistenceAccess.cpp src/SceneAlgorithms.cpp
        src/edits/SceneObjectEdit.cpp src/edits/SceneFieldEdit.cpp src/edits/SceneConfigurationEdit.cpp)
component_include_directories(scene_model_legacy
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_model_legacy PUBLIC lux::engine::editor::edit_sessions_legacy
    lux::engine::scene::scene_asset lux::engine::scene::scene_world_materialization
    lux::engine::scene::scene_camera lux::engine::simulation::ecs::transform
    lux::engine::simulation::ecs::visual lux::engine::core::type_info)
component_add_transitive_commands(scene_model_legacy
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions_legacy)"
    "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)"
    "find_package(lux-engine-scene-world-materialization REQUIRED COMPONENTS scene_world_materialization)"
    "find_package(lux-engine-scene-camera REQUIRED COMPONENTS scene_camera)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS transform visual)"
    "find_package(lux-engine-core REQUIRED COMPONENTS type_info)")
lux_classify_target(TARGET scene_model_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-model VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_model_legacy)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_model_test_legacy test/scene_session.cpp test/plugin_snapshot.cpp)
    target_link_libraries(editor_scene_model_test_legacy PRIVATE scene_model_legacy)
    lux_editor_test_options(editor_scene_model_test_legacy)
    foreach(scenario snapshot-cost applicability content atomic identity changes plugin mixed-1 mixed-2 mixed-3 mixed-4 read-5 read-6 read-7 read-8)
        add_test(NAME editor.scene_model_legacy.${scenario} COMMAND editor_scene_model_test_legacy ${scenario})
        set_tests_properties(editor.scene_model_legacy.${scenario} PROPERTIES LABELS "editor;native;P02")
    endforeach()
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_hierarchy_benchmark_legacy test/hierarchy_benchmark.cpp)
    target_link_libraries(editor_hierarchy_benchmark_legacy PRIVATE scene_model_legacy)
    lux_editor_test_options(editor_hierarchy_benchmark_legacy)
    foreach(shape chain wide roots cycle)
        add_test(NAME editor.scene_model_legacy.hierarchy_${shape} COMMAND editor_hierarchy_benchmark_legacy 50000 ${shape} 1)
        set_tests_properties(editor.scene_model_legacy.hierarchy_${shape} PROPERTIES LABELS "editor;native;P10Q" TIMEOUT 60)
    endforeach()
endif()
