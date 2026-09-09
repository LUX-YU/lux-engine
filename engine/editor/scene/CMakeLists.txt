find_package(Eigen3 CONFIG REQUIRED)
generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_SCENE_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/scene/visibility.h
)
add_component(
    COMPONENT_NAME editor_scene
    NAMESPACE lux::engine::editor
    SOURCE_FILES src/SceneCamera.cpp src/SceneWorkbench.cpp
)
component_include_directories(editor_scene BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_compile_definitions(editor_scene PRIVATE LUX_EDITOR_SCENE_LIBRARY)
target_compile_definitions(editor_scene PRIVATE LUX_SV1_DIAGNOSTICS=$<BOOL:${BUILD_TESTING}>)
target_link_libraries(editor_scene PUBLIC Eigen3::Eigen lux::engine::editor::editor_inspector
    lux::engine::scene::scene_render
    PRIVATE lux::engine::simulation::transform_system lux::engine::scene::scene_render_meta
    lux::engine::function::render_client_meta lux::engine::core::task)
component_add_transitive_commands(editor_scene
    "find_package(Eigen3 CONFIG REQUIRED)"
    "find_package(lux-engine-editor-inspector REQUIRED COMPONENTS editor_inspector)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
)
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_scene_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_scene_transform)
engine_target_add_type_runtime_info(TARGET editor_scene METAS editor_scene_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_classify_target(TARGET editor_scene LAYER EDITOR PRODUCT EDITOR ROLE INTEGRATION)
install_components(PROJECT_NAME lux-engine-editor-scene VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_scene)

if(BUILD_TESTING)
    add_executable(editor_scene_camera_test test/scene_camera_test.cpp)
    target_link_libraries(editor_scene_camera_test PRIVATE lux::engine::editor::editor_scene)
    if(MSVC)
        target_compile_options(editor_scene_camera_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(editor_scene_camera_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(TARGET editor_scene_camera_test LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
    add_test(NAME editor_scene_camera COMMAND editor_scene_camera_test)
    add_executable(editor_scene_workbench_protocol_test test/scene_workbench_protocol_test.cpp)
    target_include_directories(editor_scene_workbench_protocol_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
    target_link_libraries(editor_scene_workbench_protocol_test PRIVATE lux::engine::editor::editor_scene)
    if(MSVC)
        target_compile_options(editor_scene_workbench_protocol_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(editor_scene_workbench_protocol_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(TARGET editor_scene_workbench_protocol_test LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
    add_test(NAME editor_scene_workbench_protocol COMMAND editor_scene_workbench_protocol_test)
endif()
