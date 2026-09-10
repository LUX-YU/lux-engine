generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_SCENE_SESSION_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_SCENE_SESSION_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_SCENE_SESSION_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/sessions/scene/visibility.h)
add_component(COMPONENT_NAME editor_scene_session NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_scene_session
    SOURCE_FILES src/SceneSession.cpp src/SceneResources.cpp src/SceneCamera.cpp src/SceneView.cpp)
component_include_directories(editor_scene_session
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_compile_definitions(editor_scene_session PRIVATE LUX_EDITOR_SCENE_SESSION_LIBRARY)
target_link_libraries(editor_scene_session PUBLIC
    lux::engine::editor::editor_session_types lux::engine::editor::editor_editing
    lux::engine::editor::editor_rendering lux::engine::process::process_asset_loading
    PRIVATE lux::engine::core::task lux::engine::simulation::ecs::hierarchy)
component_add_transitive_commands(editor_scene_session
    "find_package(lux-engine-editor-session-types CONFIG REQUIRED)"
    "find_package(lux-engine-editor-editing REQUIRED COMPONENTS editor_editing)"
    "find_package(lux-engine-editor-rendering REQUIRED COMPONENTS editor_rendering)"
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)")
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_scene_session_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/sessions/scene/SceneSession.hpp
    REGISTER_FUNC_NAME lux_editor_scene_session)
engine_add_type_runtime_info(NAME editor_scene_view_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/sessions/scene/SceneView.hpp
    REGISTER_FUNC_NAME lux_editor_scene_view)
engine_target_add_type_runtime_info(TARGET editor_scene_session METAS editor_scene_session_meta editor_scene_view_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_classify_target(TARGET editor_scene_session LAYER EDITOR PRODUCT EDITOR ROLE INTEGRATION)
install_components(PROJECT_NAME lux-engine-editor-scene-session VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_scene_session)
if(LUX_EDITOR_DIAGNOSTICS)
    target_include_directories(editor_scene_session PRIVATE
        ${PROJECT_SOURCE_DIR}/engine/editor/rendering/pinclude)
    add_executable(editor_resource_completion_test test/ResourceCompletionTest.cpp)
    target_include_directories(editor_resource_completion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
    target_link_libraries(editor_resource_completion_test PRIVATE lux::engine::editor::editor_scene_session)
    target_compile_definitions(editor_resource_completion_test PRIVATE LUX_EDITOR_SCENE_TEST_DIAGNOSTICS)
    target_compile_options(editor_resource_completion_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
    lux_classify_target(TARGET editor_resource_completion_test LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
    target_compile_definitions(editor_scene_session PRIVATE LUX_EDITOR_SCENE_TEST_DIAGNOSTICS)
    target_sources(editor_scene_session PRIVATE ${PROJECT_SOURCE_DIR}/cmake/diagnostics/EditorAllocationDiagnostics.cpp)
    target_compile_definitions(editor_scene_session PRIVATE
        LUX_ALLOCATION_ARM=lux_er1_scene_allocation_fail_after
        LUX_ALLOCATION_DISARM=lux_er1_scene_allocation_disarm)
endif()
if(BUILD_TESTING)
    add_executable(editor_scene_session_test test/SceneSessionTest.cpp)
    target_include_directories(editor_scene_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
    target_link_libraries(editor_scene_session_test PRIVATE lux::engine::editor::editor_scene_session)
    target_compile_options(editor_scene_session_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
    lux_classify_target(TARGET editor_scene_session_test LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
    add_test(NAME editor_scene_session_protocol COMMAND editor_scene_session_test)
endif()
