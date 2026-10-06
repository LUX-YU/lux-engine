add_component(COMPONENT_NAME editor_composition_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/UiRegistry.cpp src/EditorContext.cpp src/CommandMenu.cpp src/ViewCommands.cpp src/ReviewView.cpp)
component_include_directories(editor_composition_legacy BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude INSTALL_TIME include)
target_include_directories(editor_composition_legacy PRIVATE ${PROJECT_SOURCE_DIR}/editor_legacy/workbench/sinclude)
target_link_libraries(editor_composition_legacy PUBLIC lux::engine::core::services
    lux::engine::function::ui lux::engine::editor::editor_contracts_legacy lux::engine::editor::layout_model_legacy
    lux::engine::editor::editor_commands_legacy)
component_add_transitive_commands(editor_composition_legacy
    "find_package(lux-engine-core REQUIRED COMPONENTS services)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts_legacy)"
    "find_package(lux-engine-editor-layout REQUIRED COMPONENTS layout_model_legacy)"
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands_legacy)")
lux_classify_target(TARGET editor_composition_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-composition VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_composition_legacy)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/ReviewView.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/UiError.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/ViewCommands.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/UiRegistry.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/EditorContext.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/CommandMenu.hpp
    DESTINATION include/lux/engine/editor/desktop COMPONENT lux_engine_editor_composition)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/settings/SettingsPage.hpp
    DESTINATION include/lux/engine/editor/settings COMPONENT lux_engine_editor_composition)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_composition_test_legacy test/composition.cpp)
    target_link_libraries(editor_composition_test_legacy PRIVATE editor_composition_legacy)
    lux_editor_test_options(editor_composition_test_legacy)
    add_test(NAME editor.composition COMMAND editor_composition_test_legacy)
    set_tests_properties(editor.composition PROPERTIES LABELS "editor;native;EC4" TIMEOUT 60 RUN_SERIAL TRUE)
    add_library(editor_composition_plugin_legacy MODULE test/composition_plugin.cpp)
    target_link_libraries(editor_composition_plugin_legacy PRIVATE editor_composition_legacy lux::engine::process::process_execution)
    lux_editor_test_options(editor_composition_plugin_legacy)
    add_executable(editor_composition_dll_test_legacy test/composition_dll.cpp)
    target_link_libraries(editor_composition_dll_test_legacy PRIVATE editor_composition_legacy
        lux::engine::process::process_execution lux::engine::platform::dynamic_library)
    lux_editor_test_options(editor_composition_dll_test_legacy)
    add_test(NAME editor.composition.dll COMMAND editor_composition_dll_test_legacy $<TARGET_FILE:editor_composition_plugin_legacy>)
    set_tests_properties(editor.composition.dll PROPERTIES LABELS "editor;native;EC4" TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_detached_views_test_legacy test/lifecycle.cpp)
    target_link_libraries(editor_detached_views_test_legacy PRIVATE lux::engine::function::ui editor_contracts_legacy)
    lux_editor_test_options(editor_detached_views_test_legacy)
    add_test(NAME editor.detached_views COMMAND editor_detached_views_test_legacy)
    set_tests_properties(editor.detached_views PROPERTIES LABELS "editor;native;P08" TIMEOUT 60 RUN_SERIAL TRUE)
endif()

add_component(COMPONENT_NAME desktop_shell_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/WorkspaceActions.cpp src/DesktopShell.cpp src/WindowInput.cpp src/WindowOutput.cpp src/Presentation.cpp src/UiRenderSyncStage.cpp)
component_include_directories(desktop_shell_legacy BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)

target_link_libraries(desktop_shell_legacy PUBLIC lux::engine::editor::workspace_store_legacy lux::engine::editor::editor_composition_legacy lux::engine::scene::scene_render
    lux::engine::function::ui_rendering lux::engine::function::input lux::engine::platform::window)
component_add_transitive_commands(desktop_shell_legacy
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition_legacy)"
    "find_package(lux-engine-editor-workspace-store REQUIRED COMPONENTS workspace_store_legacy)"
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands_legacy)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui_rendering input)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS window)")
lux_classify_target(TARGET desktop_shell_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-desktop VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS desktop_shell_legacy)



# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WorkspaceActions.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopError.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopShell.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/Presentation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WindowInput.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WindowOutput.hpp
    DESTINATION include/lux/engine/editor/desktop COMPONENT lux_engine_editor_desktop)


if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    target_compile_definitions(desktop_shell_legacy PRIVATE LUX_DESKTOP_TEST_ACCESS=1)
endif()
