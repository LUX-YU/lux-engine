add_component(COMPONENT_NAME scene_persistence NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneSaveSource.cpp src/SceneCodec.cpp)
component_include_directories(scene_persistence
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_persistence PUBLIC lux::engine::editor::scene_model lux::engine::editor::editor_persistence)
component_add_transitive_commands(scene_persistence
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)")
lux_classify_target(TARGET scene_persistence LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_persistence)

add_component(COMPONENT_NAME editor_editing_scene NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneEditing.cpp src/ComponentEdit.cpp)
lux_classify_target(TARGET editor_editing_scene LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_editing_scene
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_editing_scene PUBLIC
    lux::engine::editor::scene_model
    lux::engine::scene::scene_composition
    lux::engine::scene::scene_world_loading lux::engine::scene::scene_render
    lux::engine::simulation::ecs::hierarchy)
component_add_transitive_commands(editor_editing_scene
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model)"
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

add_interface_component(COMPONENT_NAME scene_execution_api NAMESPACE lux::engine::editor)
component_include_directories(scene_execution_api
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_execution_api INTERFACE lux::engine::editor::scene_model
    lux::engine::scene::scene_composition lux::engine::function::render_client)
component_add_transitive_commands(scene_execution_api
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)")
lux_classify_target(TARGET scene_execution_api LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)



add_component(COMPONENT_NAME scene_execution NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/RunStore.cpp)
component_include_directories(scene_execution
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
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

add_component(COMPONENT_NAME scene_projection NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ResourceStatus.cpp src/SceneProjection.cpp)
component_include_directories(scene_projection BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_projection PUBLIC lux::engine::editor::scene_model
    lux::engine::scene::scene_composition lux::engine::scene::scene_render
    lux::engine::scene::scene_world_loading lux::engine::process::world_loading)
component_add_transitive_commands(scene_projection
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)")
lux_classify_target(TARGET scene_projection LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-projection VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_projection)


# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/editing/scene/FieldEdit.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/editing/scene/SceneEdit.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/editing/scene/SceneEditing.hpp
    DESTINATION include/lux/engine/editor/editing/scene COMPONENT lux_engine_editor_editing_scene)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/RunController.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/RunInspectAccess.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/RunStore.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/RunTypes.hpp
    DESTINATION include/lux/engine/editor/scene COMPONENT lux_engine_editor_scene_execution)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneCodec.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneSaveSource.hpp
    DESTINATION include/lux/engine/editor/scene COMPONENT lux_engine_editor_scene_persistence)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/ResourceStatus.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneProjection.hpp
    DESTINATION include/lux/engine/editor/scene COMPONENT lux_engine_editor_scene_projection)
