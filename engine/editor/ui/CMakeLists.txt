find_package(imgui REQUIRED COMPONENTS core)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_UI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/ui/visibility.h)
add_component(COMPONENT_NAME editor_ui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_ui
    SOURCE_FILES src/GuiFrontend.cpp src/shell/EditorWindow.cpp src/actions/ActiveEditHistory.cpp
        src/actions/HistoryActions.cpp src/asset/AssetReference.cpp)
component_include_directories(editor_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude INSTALL_TIME include)
target_compile_definitions(editor_ui PRIVATE LUX_EDITOR_UI_LIBRARY)
target_link_libraries(editor_ui PUBLIC lux::engine::editor::editor_core lux::engine::editor::editor_rendering
    lux::engine::function::ui imgui::core PRIVATE lux::engine::platform::window)
component_add_transitive_commands(editor_ui
    "find_package(lux-engine-editor-core REQUIRED COMPONENTS editor_core)"
    "find_package(lux-engine-editor-rendering REQUIRED COMPONENTS editor_rendering)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(imgui REQUIRED COMPONENTS core)")
if(WIN32)
    target_link_libraries(editor_ui PRIVATE imm32 comdlg32)
endif()
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_gui_history_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/gui/actions/HistoryActions.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_gui_history)
engine_add_type_runtime_info(NAME editor_gui_window_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/gui/shell/EditorWindow.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_gui_window)
engine_target_add_type_runtime_info(TARGET editor_ui METAS editor_gui_history_meta editor_gui_window_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_ui)
