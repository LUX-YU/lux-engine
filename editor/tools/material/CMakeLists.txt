find_package(node_editor REQUIRED COMPONENTS node_editor)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_MATERIAL_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_MATERIAL_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_MATERIAL_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/material/visibility.h)
add_component(COMPONENT_NAME editor_material NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_material
    SOURCE_FILES src/MaterialEditor.cpp src/MaterialPreview.cpp src/MaterialAssets.cpp)
component_include_directories(editor_material
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/editor/assets/sinclude ${PROJECT_SOURCE_DIR}/editor/transition ${PROJECT_SOURCE_DIR}/editor/storage/sinclude ${PROJECT_SOURCE_DIR}/editor/editing/sinclude INSTALL_TIME include)
target_compile_definitions(editor_material PRIVATE LUX_EDITOR_MATERIAL_LIBRARY)
target_link_libraries(editor_material PUBLIC lux::engine::editor::editor_metadata lux::engine::scene::scene_render lux::engine::editor::editor_assets lux::engine::function::material_graph
        lux::engine::editor::material_model
    PRIVATE lux::engine::editor::editor_context lux::engine::toolchain::toolchain_material_compiler lux::engine::scene::scene_transform
    lux::engine::world::world_storage lux::engine::process::asset_loading)
component_add_transitive_commands(editor_material
    "find_package(lux-engine-editor-metadata REQUIRED COMPONENTS editor_metadata)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-function REQUIRED COMPONENTS material_graph)"
    "find_package(lux-engine-editor-material-model REQUIRED COMPONENTS material_model)")
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_material_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialEditor.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_material)
engine_target_add_type_runtime_info(TARGET editor_material METAS editor_material_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
target_sources(editor_material PRIVATE src/ui/MaterialElements.cpp src/ui/MaterialPreviewElement.cpp)
lux_classify_target(TARGET editor_material LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_material
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/editor/ui/sinclude INSTALL_TIME include)
target_include_directories(editor_material PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
target_link_libraries(editor_material PUBLIC lux::engine::editor::editor_ui
    PRIVATE imgui::node_editor)
component_add_transitive_commands(editor_material
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_material)

if(LUX_EDITOR_TEST_DESKTOP_GPU)
    target_sources(editor_material PRIVATE test/MaterialEditorTestAccess.cpp)
endif()
