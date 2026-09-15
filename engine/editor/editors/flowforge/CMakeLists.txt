find_package(node_editor REQUIRED COMPONENTS node_editor)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_FLOWFORGE_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_FLOWFORGE_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_FLOWFORGE_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/flowforge/visibility.h)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_FLOWFORGE_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_FLOWFORGE_UI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_FLOWFORGE_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/flowforge/ui/visibility.h)
add_component(COMPONENT_NAME editor_flowforge NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_flowforge
    SOURCE_FILES src/FlowForgeEditor.cpp)
component_include_directories(editor_flowforge
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/engine/editor/core/sinclude INSTALL_TIME include)
target_compile_definitions(editor_flowforge PRIVATE LUX_EDITOR_FLOWFORGE_LIBRARY)
target_link_libraries(editor_flowforge PUBLIC lux::engine::editor::editor_core lux::engine::function::flowforge
    PRIVATE lux::engine::flowforge::flowforge_compiler)
component_add_transitive_commands(editor_flowforge
    "find_package(lux-engine-editor-core REQUIRED COMPONENTS editor_core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS flowforge)")
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_flow_document_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/flowforge/FlowForgeEditor.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_flow_document)
engine_target_add_type_runtime_info(TARGET editor_flowforge METAS editor_flow_document_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
add_component(COMPONENT_NAME editor_flowforge_ui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_flowforge_ui
    SOURCE_FILES src/ui/FlowForgeDocumentProvider.cpp)
component_include_directories(editor_flowforge_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/engine/editor/ui/sinclude INSTALL_TIME include)
target_compile_definitions(editor_flowforge_ui PRIVATE LUX_EDITOR_FLOWFORGE_UI_LIBRARY)
target_link_libraries(editor_flowforge_ui PUBLIC lux::engine::editor::editor_flowforge lux::engine::editor::editor_ui
    PRIVATE imgui::node_editor)
component_add_transitive_commands(editor_flowforge_ui
    "find_package(lux-engine-editor-flowforge REQUIRED COMPONENTS editor_flowforge)"
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_flowforge editor_flowforge_ui)
