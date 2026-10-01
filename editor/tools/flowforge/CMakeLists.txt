find_package(node_editor REQUIRED COMPONENTS node_editor)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_FLOWFORGE_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_FLOWFORGE_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_FLOWFORGE_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/flowforge/visibility.h)
add_component(COMPONENT_NAME editor_flowforge STATIC NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_flowforge
    SOURCE_FILES src/FlowForgeEditor.cpp src/FlowAssets.cpp)
component_include_directories(editor_flowforge
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/editor/assets/sinclude ${PROJECT_SOURCE_DIR}/editor/transition ${PROJECT_SOURCE_DIR}/editor/storage/sinclude ${PROJECT_SOURCE_DIR}/editor/editing/sinclude INSTALL_TIME include)
target_compile_definitions(editor_flowforge PRIVATE LUX_EDITOR_FLOWFORGE_LIBRARY)
target_link_libraries(editor_flowforge PUBLIC lux::engine::editor::editor_assets lux::engine::function::flowforge
    PRIVATE lux::engine::editor::flowforge_compilation lux::engine::editor::editor_context lux::engine::flowforge::flowforge_compiler)
component_add_transitive_commands(editor_flowforge
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-function REQUIRED COMPONENTS flowforge)")
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_flow_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/flowforge/FlowForgeEditor.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_flow)
engine_target_add_type_runtime_info(TARGET editor_flowforge METAS editor_flow_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
target_sources(editor_flowforge PRIVATE src/ui/FlowForgeElements.cpp)
lux_classify_target(TARGET editor_flowforge LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_flowforge
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/editor/ui/sinclude INSTALL_TIME include)
target_link_libraries(editor_flowforge PUBLIC lux::engine::editor::editor_ui
    PRIVATE imgui::node_editor lux::engine::editor::editor_metadata)
component_add_transitive_commands(editor_flowforge
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_flowforge)

if(LUX_EDITOR_TEST_DESKTOP_GPU)
    target_sources(editor_flowforge PRIVATE test/FlowForgeEditorTestAccess.cpp)
endif()

target_link_libraries(editor_flowforge PUBLIC lux::engine::editor::flowforge_model)
component_add_transitive_commands(editor_flowforge
    "find_package(lux-engine-editor-flowforge-model REQUIRED COMPONENTS flowforge_model)")

target_link_libraries(editor_flowforge PUBLIC lux::engine::editor::flow_ui)
component_add_transitive_commands(editor_flowforge "find_package(lux-engine-editor-flow-ui REQUIRED COMPONENTS flow_ui)")

target_compile_definitions(editor_flowforge PUBLIC LUX_EDITOR_FLOWFORGE_DLL_DISABLE)
