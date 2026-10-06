add_component(COMPONENT_NAME scene_persistence_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneSaveSource.cpp src/SceneCodec.cpp src/SessionFactory.cpp)
component_include_directories(scene_persistence_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_persistence_legacy PUBLIC lux::engine::editor::scene_model_legacy lux::engine::editor::editor_persistence_legacy)
component_add_transitive_commands(scene_persistence_legacy
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model_legacy)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_legacy)")
target_link_libraries(scene_persistence_legacy PUBLIC lux::engine::editor::session_factories_legacy)
target_include_directories(scene_persistence_legacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sessions/sinclude)
component_add_transitive_commands(scene_persistence_legacy
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories_legacy)")
target_link_libraries(scene_persistence_legacy PRIVATE lux::engine::core::services)
component_add_transitive_commands(scene_persistence_legacy "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET scene_persistence_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_persistence_legacy)

add_component(COMPONENT_NAME editor_editing_scene_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneEditing.cpp src/ComponentEdit.cpp)
lux_classify_target(TARGET editor_editing_scene_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_editing_scene_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_editing_scene_legacy PUBLIC
    lux::engine::editor::scene_model_legacy
    lux::engine::scene::scene_composition
    lux::engine::scene::scene_world_loading lux::engine::scene::scene_render
    lux::engine::simulation::ecs::hierarchy)
component_add_transitive_commands(editor_editing_scene_legacy
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model_legacy)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS hierarchy)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-editing-scene VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_editing_scene_legacy)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_editing_test_legacy test/editing.cpp)
    target_link_libraries(editor_scene_editing_test_legacy PRIVATE editor_editing_scene_legacy)
    if(MSVC)
        target_compile_options(editor_scene_editing_test_legacy PRIVATE /UNDEBUG)
    else()
        target_compile_options(editor_scene_editing_test_legacy PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.scene.editing COMMAND editor_scene_editing_test_legacy)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    set_tests_properties(editor.scene.editing
        PROPERTIES LABELS "editor;native")
endif()

add_interface_component(COMPONENT_NAME scene_execution_api_legacy NAMESPACE lux::engine::editor)
component_include_directories(scene_execution_api_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_execution_api_legacy INTERFACE lux::engine::editor::scene_model_legacy
    lux::engine::scene::scene_composition lux::engine::function::render_client)
component_add_transitive_commands(scene_execution_api_legacy
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model_legacy)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)")
lux_classify_target(TARGET scene_execution_api_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)



add_component(COMPONENT_NAME scene_execution_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneConfigurationPreparation.cpp src/RunStore.cpp src/ModelCreationOperation.cpp)


target_link_libraries(scene_execution_legacy PRIVATE lux::engine::scene::scene_transform lux::engine::core::services)
component_add_transitive_commands(scene_execution_legacy "find_package(lux-engine-core REQUIRED COMPONENTS services)")
target_link_libraries(scene_execution_legacy PRIVATE lux::engine::scene::scene_script_runtime)
component_add_transitive_commands(scene_execution_legacy
    "find_package(lux-engine-scene-script-runtime REQUIRED COMPONENTS scene_script_runtime)")
component_add_transitive_commands(scene_execution_legacy "find_package(lux-engine-scene-transform REQUIRED COMPONENTS scene_transform)")
component_include_directories(scene_execution_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_execution_legacy PUBLIC lux::engine::editor::scene_execution_api_legacy
    lux::engine::editor::editor_editing_scene_legacy lux::engine::scene::scene_render
    lux::engine::scene::scene_world_loading lux::engine::process::world_loading
    lux::engine::editor::editor_project_legacy lux::engine::process::asset_loading)
component_add_transitive_commands(scene_execution_legacy
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_execution_api_legacy)"
    "find_package(lux-engine-editor-project REQUIRED COMPONENTS editor_project_legacy)"
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)"
    "find_package(lux-engine-editor-editing-scene REQUIRED COMPONENTS editor_editing_scene_legacy)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)")
lux_classify_target(TARGET scene_execution_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
# Project-backed placement is a separate dependency closure from Run/Scene execution.
# Runtime and operation consumers do not acquire project publication, plugins or platform IO.
add_component(COMPONENT_NAME scene_project_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ModelPlacementService.cpp src/ProjectSceneEnvironment.cpp)
component_include_directories(scene_project_legacy BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include)
target_link_libraries(scene_project_legacy PUBLIC lux::engine::editor::scene_execution_legacy
    PRIVATE lux::engine::editor::editor_storage_legacy lux::engine::core::services lux::engine::editor::scene_projection_legacy)
component_add_transitive_commands(scene_project_legacy
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_execution_legacy)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage_legacy)"
    "find_package(lux-engine-editor-scene-projection REQUIRED COMPONENTS scene_projection_legacy)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET scene_project_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-execution VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_execution_api_legacy scene_execution_legacy scene_project_legacy)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_execution_test_legacy test/runs.cpp)
    target_link_libraries(editor_scene_execution_test_legacy PRIVATE scene_execution_legacy lux::engine::scene::scene_transform lux::engine::core::services)
    lux_editor_test_options(editor_scene_execution_test_legacy)
    foreach(scenario isolation controls failure completion r1-queued r1-mixed r1-failed r1-callback r1-capacity model-insertion service)
        add_test(NAME editor.scene_execution_legacy.${scenario} COMMAND editor_scene_execution_test_legacy ${scenario})
        set_tests_properties(editor.scene_execution_legacy.${scenario} PROPERTIES LABELS "editor;native;P06")
    endforeach()
    set_tests_properties(editor.scene_execution_legacy.service PROPERTIES LABELS "editor;native;EC4")
    set_tests_properties(editor.scene_execution_legacy.model-insertion PROPERTIES LABELS "editor;native;P12")
endif()

add_component(COMPONENT_NAME scene_projection_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ResourceStatus.cpp src/SceneProjection.cpp)
component_include_directories(scene_projection_legacy BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_projection_legacy PUBLIC lux::engine::core::services lux::engine::editor::scene_model_legacy
    lux::engine::scene::scene_composition lux::engine::scene::scene_render
    lux::engine::scene::scene_world_loading lux::engine::process::world_loading)
component_add_transitive_commands(scene_projection_legacy
    "find_package(lux-engine-core REQUIRED COMPONENTS services)"
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model_legacy)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)")
lux_classify_target(TARGET scene_projection_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-projection VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_projection_legacy)


# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/editing/scene/FieldEdit.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/editing/scene/SceneEdit.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/editing/scene/SceneEditing.hpp
    DESTINATION include/lux/engine/editor/editing/scene COMPONENT lux_engine_editor_editing_scene)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneConfigurationPreparation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/ModelCreationOperation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/ModelPlacement.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/StartRunOperation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/RunInspectAccess.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/RunStore.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/RunTypes.hpp
    DESTINATION include/lux/engine/editor/scene COMPONENT lux_engine_editor_scene_execution)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/ModelPlacementService.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/ProjectSceneEnvironment.hpp
    DESTINATION include/lux/engine/editor/scene COMPONENT lux_engine_editor_scene_project)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneCodec.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneSaveSource.hpp
    DESTINATION include/lux/engine/editor/scene COMPONENT lux_engine_editor_scene_persistence)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/ResourceStatus.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneProjection.hpp
    DESTINATION include/lux/engine/editor/scene COMPONENT lux_engine_editor_scene_projection)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneSessionFactory.hpp
    DESTINATION include/lux/engine/editor/scene COMPONENT lux_engine_editor_scene_persistence)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_model_placement_test_legacy test/model_placement.cpp)
    target_link_libraries(editor_model_placement_test_legacy PRIVATE scene_project_legacy scene_projection_legacy editor_storage_legacy lux::engine::core::services
        lux::engine::toolchain::toolchain_material_cooker)
    lux_editor_test_options(editor_model_placement_test_legacy)
    add_test(NAME editor.scene.model-placement COMMAND editor_model_placement_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/placements")
    set_tests_properties(editor.scene.model-placement PROPERTIES LABELS "editor;native;EC4" RUN_SERIAL TRUE TIMEOUT 60)
endif()
