find_package(imgui REQUIRED COMPONENTS core)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_UI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/ui/visibility.h)
add_component(COMPONENT_NAME editor_ui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_ui
    SOURCE_FILES src/UIRenderSystem.cpp src/actions/ActiveEditHistory.cpp
        src/actions/HistoryActions.cpp src/asset/AssetReference.cpp)
component_include_directories(editor_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        ${PROJECT_SOURCE_DIR}/modules/function/ui/pinclude ${PROJECT_SOURCE_DIR}/modules/function/ui/sinclude INSTALL_TIME include)
target_compile_definitions(editor_ui PRIVATE LUX_EDITOR_UI_LIBRARY)
target_link_libraries(editor_ui PUBLIC lux::engine::editor::editor_core
    lux::engine::function::ui lux::engine::function::ui_rendering lux::engine::scene::scene_render imgui::core)
component_add_transitive_commands(editor_ui
    "find_package(lux-engine-editor-core REQUIRED COMPONENTS editor_core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui ui_rendering)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(imgui REQUIRED COMPONENTS core)")
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_gui_history_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/gui/actions/HistoryActions.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_gui_history)
engine_add_type_runtime_info(NAME editor_ui_system_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/ui/UIRenderSystem.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing REGISTER_FUNC_NAME lux_editor_ui_system)
engine_target_add_type_runtime_info(TARGET editor_ui METAS editor_gui_history_meta editor_ui_system_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_ui)

if(BUILD_TESTING)
    add_executable(editor_ui_foundation_test
        ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2/protocol/foundation_protocol.cpp)
    target_link_libraries(editor_ui_foundation_test PRIVATE editor_ui)
    if(MSVC)
        target_compile_options(editor_ui_foundation_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(editor_ui_foundation_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.ui_foundation COMMAND editor_ui_foundation_test)
    add_executable(editor_unified_ui_test test/unified_ui.cpp)
    target_link_libraries(editor_unified_ui_test PRIVATE editor_ui lux::engine::platform::window)
    if(MSVC)
        target_compile_options(editor_unified_ui_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(editor_unified_ui_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.unified_ui COMMAND editor_unified_ui_test)
    add_test(NAME editor.unified_ui_multiple COMMAND editor_unified_ui_test --multiple)
    if(WIN32)
        add_test(NAME editor.unified_ui_surface COMMAND editor_unified_ui_test --surface)
    endif()
endif()
