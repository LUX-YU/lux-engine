add_component(COMPONENT_NAME editor_editing_scene NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneEditing.cpp src/ComponentEdit.cpp)
lux_classify_target(TARGET editor_editing_scene LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_editing_scene
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_editing_scene PUBLIC
    lux::engine::editor::editor_storage lux::engine::scene::scene_composition
    lux::engine::scene::scene_world_loading lux::engine::scene::scene_render
    lux::engine::simulation::ecs::hierarchy)
component_add_transitive_commands(editor_editing_scene
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS hierarchy)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-editing-scene VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_editing_scene)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_editing_test test/editing.cpp)
    target_link_libraries(editor_scene_editing_test PRIVATE editor_editing_scene)
    if(MSVC)
        target_compile_options(editor_scene_editing_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(editor_scene_editing_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.scene.editing COMMAND editor_scene_editing_test)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    set_tests_properties(editor.scene.editing
        PROPERTIES LABELS "editor;native")
endif()
