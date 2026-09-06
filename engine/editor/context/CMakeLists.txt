## Editor-wide L5 capability root and typed L4 Tool container.

find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_EDITOR_CONTEXT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_EDITOR_CONTEXT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_EDITOR_CONTEXT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/editor/context/visibility.h
)

add_component(
    COMPONENT_NAME editor_context
    NAMESPACE      lux::engine::editor
    OUTPUT_NAME    lux_engine_editor_context
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EditorContext.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EditorSelection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Toolset.cpp
)

component_include_directories(
    editor_context
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME
        include
)

target_compile_definitions(editor_context PRIVATE LUX_EDITOR_CONTEXT_LIBRARY)

target_link_libraries(
    editor_context
    PUBLIC
        lux::cxx::compile_time
        lux::engine::function::ui
        lux::engine::resource::asset
        lux::engine::process::process_execution
        lux::engine::process::asset_loading
        lux::engine::scene::scene_meta
    PRIVATE
        lux::engine::scene::scene_composition
)

component_add_transitive_commands(
    editor_context
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)"
    "find_package(lux-engine-scene-meta REQUIRED COMPONENTS scene_meta)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
)

include_component_cmake_scripts(meta)
engine_add_type_runtime_info(
    NAME editor_selection_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/EditorSelection.hpp
    REGISTER_FUNC_NAME lux_editor_selection
)
engine_target_add_type_runtime_info(
    TARGET editor_context
    METAS editor_selection_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)

lux_classify_target(
    TARGET  editor_context
    LAYER   EDITOR
    PRODUCT EDITOR
    ROLE    FOUNDATION
)

install_components(
    PROJECT_NAME lux-engine-editor-context
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::editor
    COMPONENTS   editor_context
)
