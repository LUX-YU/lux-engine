generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_DOCUMENTS_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_DOCUMENTS_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_DOCUMENTS_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/documents/visibility.h
)
add_component(COMPONENT_NAME editor_documents NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_documents
    SOURCE_FILES src/scene/NativeScene.cpp src/scene/SceneEditor.cpp src/scene/SceneOpening.cpp src/scene/SceneResources.cpp
)
component_include_directories(editor_documents
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(editor_documents PRIVATE LUX_EDITOR_DOCUMENTS_LIBRARY)
target_link_libraries(editor_documents PUBLIC
    lux::engine::editor::editor_core lux::engine::editor::editor_rendering
    lux::engine::scene::scene_composition lux::engine::scene::scene_meta
    lux::engine::scene::scene_world_materialization lux::engine::process::world_loading
    lux::engine::scene::scene_asset lux::engine::world::world_asset lux::engine::simulation::simulation_asset
    lux::engine::simulation::ecs::hierarchy
)
component_add_transitive_commands(editor_documents
    "find_package(lux-engine-editor-core REQUIRED COMPONENTS editor_core)"
    "find_package(lux-engine-editor-rendering REQUIRED COMPONENTS editor_rendering)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-meta REQUIRED COMPONENTS scene_meta)"
    "find_package(lux-engine-scene-world-materialization REQUIRED COMPONENTS scene_world_materialization)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)"
    "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)"
    "find_package(lux-engine-world-asset REQUIRED COMPONENTS world_asset)"
    "find_package(lux-engine-simulation-asset REQUIRED COMPONENTS simulation_asset)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS hierarchy)"
)
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_scene_document_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneEditor.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing
    REGISTER_FUNC_NAME lux_editor_scene_document)
engine_target_add_type_runtime_info(TARGET editor_documents METAS editor_scene_document_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
install_components(PROJECT_NAME lux-engine-editor-documents VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_documents)
