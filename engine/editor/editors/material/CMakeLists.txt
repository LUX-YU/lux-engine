find_package(node_editor REQUIRED COMPONENTS node_editor)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_MATERIAL_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_MATERIAL_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_MATERIAL_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/material/visibility.h)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_MATERIAL_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_MATERIAL_UI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_MATERIAL_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/material/ui/visibility.h)
add_component(COMPONENT_NAME editor_material NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_material
    SOURCE_FILES src/MaterialEditor.cpp)
component_include_directories(editor_material
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/engine/editor/core/sinclude INSTALL_TIME include)
target_compile_definitions(editor_material PRIVATE LUX_EDITOR_MATERIAL_LIBRARY)
target_link_libraries(editor_material PUBLIC lux::engine::editor::editor_core lux::engine::function::material_graph
    PRIVATE lux::engine::toolchain::toolchain_material_compiler)
component_add_transitive_commands(editor_material
    "find_package(lux-engine-editor-core REQUIRED COMPONENTS editor_core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS material_graph)")
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_material_document_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialEditor.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_material_document)
engine_target_add_type_runtime_info(TARGET editor_material METAS editor_material_document_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
add_component(COMPONENT_NAME editor_material_ui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_material_ui
    SOURCE_FILES src/ui/MaterialDocumentProvider.cpp)
component_include_directories(editor_material_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/engine/editor/ui/sinclude INSTALL_TIME include)
target_compile_definitions(editor_material_ui PRIVATE LUX_EDITOR_MATERIAL_UI_LIBRARY)
target_link_libraries(editor_material_ui PUBLIC lux::engine::editor::editor_material lux::engine::editor::editor_ui
    PRIVATE imgui::node_editor)
component_add_transitive_commands(editor_material_ui
    "find_package(lux-engine-editor-material REQUIRED COMPONENTS editor_material)"
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_material editor_material_ui)
