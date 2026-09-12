generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_UI_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/ui/visibility.h
)
add_component(
    COMPONENT_NAME editor_ui
    NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_ui
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/shell/EditorWindow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/actions/ActiveEditHistory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/actions/HistoryActions.cpp
)
component_include_directories(editor_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(editor_ui PRIVATE LUX_EDITOR_UI_LIBRARY)
target_link_libraries(editor_ui PUBLIC
    lux::engine::editor::editor_editing
    lux::engine::function::ui
    PRIVATE lux::engine::platform::window
)
component_add_transitive_commands(editor_ui
    "find_package(lux-engine-editor-editing REQUIRED COMPONENTS editor_editing)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
)
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(
    NAME editor_history_actions_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/ui/actions/HistoryActions.hpp
    REGISTER_FUNC_NAME lux_editor_history_actions
)
engine_target_add_type_runtime_info(TARGET editor_ui METAS editor_history_actions_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)

install_components(PROJECT_NAME lux-engine-editor-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_ui
)
if(WIN32)
    target_link_libraries(editor_ui PRIVATE imm32)
endif()
