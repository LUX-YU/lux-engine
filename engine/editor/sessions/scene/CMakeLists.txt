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

install_components(PROJECT_NAME lux-engine-editor-scene-session VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_scene_session)
