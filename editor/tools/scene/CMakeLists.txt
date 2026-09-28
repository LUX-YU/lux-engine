generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_SCENE_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/scene/visibility.h
)
add_component(COMPONENT_NAME editor_scene NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_scene
    SOURCE_FILES src/SceneSource.cpp src/SceneAssets.cpp src/SceneEditor.cpp src/SceneContent.cpp src/SceneObjectEdits.cpp src/ModelCreation.cpp src/SceneOpening.cpp src/ScenePlayback.cpp
)
component_include_directories(editor_scene
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/editor/assets/sinclude ${PROJECT_SOURCE_DIR}/editor/storage/sinclude ${PROJECT_SOURCE_DIR}/editor/editing/sinclude
    INSTALL_TIME include
)
target_compile_definitions(editor_scene PRIVATE LUX_EDITOR_SCENE_LIBRARY)
target_link_libraries(editor_scene PUBLIC
    lux::engine::editor::editor_editing_scene
    lux::engine::editor::editor_assets lux::engine::editor::editor_metadata lux::engine::scene::scene_render lux::engine::function::render_runtime
    lux::engine::scene::scene_composition
    lux::engine::scene::scene_world_materialization lux::engine::process::world_loading
    lux::engine::scene::scene_world_loading
    lux::engine::scene::scene_asset lux::engine::world::world_asset lux::engine::simulation::simulation_asset
    lux::engine::simulation::ecs::hierarchy
    lux::engine::scene::scene_mesh_query
    PRIVATE lux::engine::editor::editor_context lux::cxx::concurrent lux::engine::scene::scene_transform
          lux::engine::function::render_features
)
component_add_transitive_commands(editor_scene
    "find_package(lux-engine-editor-editing-scene REQUIRED COMPONENTS editor_editing_scene)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-editor-metadata REQUIRED COMPONENTS editor_metadata)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_runtime)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-world-materialization REQUIRED COMPONENTS scene_world_materialization)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)"
    "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)"
    "find_package(lux-engine-world-asset REQUIRED COMPONENTS world_asset)"
    "find_package(lux-engine-simulation-asset REQUIRED COMPONENTS simulation_asset)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS hierarchy)"
    "find_package(lux-engine-scene-mesh-query REQUIRED COMPONENTS scene_mesh_query)"
)
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_scene_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneEditor.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing
    REGISTER_FUNC_NAME lux_editor_scene)
engine_target_add_type_runtime_info(TARGET editor_scene METAS editor_scene_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)

find_package(imgui REQUIRED COMPONENTS core)
target_sources(editor_scene PRIVATE src/ui/SceneCreationPane.cpp src/ui/SceneUi.cpp src/ui/InspectorPane.cpp src/ui/InspectorElement.cpp
        src/ui/SceneContentElement.cpp
        src/ui/OutlinerPane.cpp src/ui/ResourcePane.cpp)
lux_classify_target(TARGET editor_scene LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_scene
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/editor/ui/sinclude INSTALL_TIME include)
option(LUX_EDITOR_UI_DIAGNOSTICS "Private UI work counters; exclude from normal SDK and timing" OFF)
if(LUX_EDITOR_UI_DIAGNOSTICS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        message(FATAL_ERROR "Editor UI diagnostics require RelWithDebInfo")
    endif()
    target_compile_definitions(editor_scene PRIVATE LUX_EDITOR_UI_DIAGNOSTICS)
endif()
target_link_libraries(editor_scene PUBLIC lux::engine::editor::editor_ui lux::engine::editor::editor_assets lux::engine::function::ui imgui::core
    PRIVATE lux::engine::platform::window lux::engine::scene::scene_transform
     )
component_add_transitive_commands(editor_scene
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(imgui REQUIRED COMPONENTS core)")
if(WIN32)
    target_link_libraries(editor_scene PRIVATE imm32 comdlg32)
endif()
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_scene_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/scene/builtin_systems/transform/include/lux/engine/scene/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_scene_transform)
engine_target_add_type_runtime_info(TARGET editor_scene METAS editor_scene_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_scene)

if(LUX_EDITOR_TEST_DESKTOP_GPU)
    add_executable(editor_scene_fixture test/fixture.cpp)
    target_link_libraries(editor_scene_fixture PRIVATE editor_scene lux::engine::function::render_features
        lux::engine::scene::scene_transform)
    if(MSVC)
        target_compile_options(editor_scene_fixture PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_scene_fixture PRIVATE -UNDEBUG)
    endif()
endif()

if(LUX_EDITOR_TEST_DESKTOP_GPU)
    target_sources(editor_scene PRIVATE test/SceneEditorTestAccess.cpp)
endif()
