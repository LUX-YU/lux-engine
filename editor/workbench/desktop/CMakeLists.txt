add_component(COMPONENT_NAME editor_composition NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/UiRegistry.cpp src/EditorContext.cpp src/CommandMenu.cpp src/ViewCommands.cpp)
component_include_directories(editor_composition BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude INSTALL_TIME include)
target_include_directories(editor_composition PRIVATE ${PROJECT_SOURCE_DIR}/editor/workbench/sinclude)
target_link_libraries(editor_composition PUBLIC lux::engine::core::services
    lux::engine::function::ui lux::engine::editor::editor_contracts lux::engine::editor::layout_model
    lux::engine::editor::editor_commands)
component_add_transitive_commands(editor_composition
    "find_package(lux-engine-core REQUIRED COMPONENTS services)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts)"
    "find_package(lux-engine-editor-layout REQUIRED COMPONENTS layout_model)"
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands)")
lux_classify_target(TARGET editor_composition LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-composition VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_composition)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/ViewCommands.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/UiRegistry.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/EditorContext.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/CommandMenu.hpp
    DESTINATION include/lux/engine/editor/desktop COMPONENT lux_engine_editor_composition)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/settings/SettingsPage.hpp
    DESTINATION include/lux/engine/editor/settings COMPONENT lux_engine_editor_composition)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_composition_test test/composition.cpp)
    target_link_libraries(editor_composition_test PRIVATE editor_composition)
    lux_editor_test_options(editor_composition_test)
    add_test(NAME editor.composition COMMAND editor_composition_test)
    set_tests_properties(editor.composition PROPERTIES LABELS "editor;native;EC4" TIMEOUT 60 RUN_SERIAL TRUE)
    add_library(editor_composition_plugin MODULE test/composition_plugin.cpp)
    target_link_libraries(editor_composition_plugin PRIVATE editor_composition lux::engine::process::process_execution)
    lux_editor_test_options(editor_composition_plugin)
    add_executable(editor_composition_dll_test test/composition_dll.cpp)
    target_link_libraries(editor_composition_dll_test PRIVATE editor_composition
        lux::engine::process::process_execution lux::engine::platform::dynamic_library)
    lux_editor_test_options(editor_composition_dll_test)
    add_test(NAME editor.composition.dll COMMAND editor_composition_dll_test $<TARGET_FILE:editor_composition_plugin>)
    set_tests_properties(editor.composition.dll PROPERTIES LABELS "editor;native;EC4" TIMEOUT 60 RUN_SERIAL TRUE)
endif()

add_component(COMPONENT_NAME view_api NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/ViewFactory.cpp)
component_include_directories(view_api BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude INSTALL_TIME include)
target_link_libraries(view_api PUBLIC lux::engine::function::ui lux::engine::editor::editor_contracts lux::engine::editor::layout_model)
component_add_transitive_commands(view_api
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts)"
    "find_package(lux-engine-editor-layout REQUIRED COMPONENTS layout_model)")
lux_classify_target(TARGET view_api LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-view-api VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS view_api)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_detached_views_test test/lifecycle.cpp)
    target_link_libraries(editor_detached_views_test PRIVATE view_api)
    lux_editor_test_options(editor_detached_views_test)
    add_test(NAME editor.detached_views COMMAND editor_detached_views_test)
    set_tests_properties(editor.detached_views PROPERTIES LABELS "editor;native;P08" TIMEOUT 60 RUN_SERIAL TRUE)
endif()

add_component(COMPONENT_NAME view_host NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/ViewHost.cpp src/ReviewView.cpp)
component_include_directories(view_host BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude INSTALL_TIME include)
target_include_directories(view_host PRIVATE ${PROJECT_SOURCE_DIR}/editor/workbench/sinclude)
target_link_libraries(view_host PUBLIC lux::engine::editor::editor_commands lux::engine::editor::view_api lux::engine::editor::layout_model)
component_add_transitive_commands(view_host
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands)"
    "find_package(lux-engine-editor-view-api REQUIRED COMPONENTS view_api)"
    "find_package(lux-engine-editor-layout REQUIRED COMPONENTS layout_model)")
lux_classify_target(TARGET view_host LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
add_component(COMPONENT_NAME desktop_shell NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/WorkspaceActions.cpp src/DesktopShell.cpp src/WindowInput.cpp src/WindowOutput.cpp src/Presentation.cpp src/UiRenderSyncStage.cpp)
component_include_directories(desktop_shell BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)

target_link_libraries(desktop_shell PUBLIC lux::engine::editor::workspace_store lux::engine::editor::editor_composition lux::engine::scene::scene_render
    lux::engine::function::ui_rendering lux::engine::function::input lux::engine::platform::window)
component_add_transitive_commands(desktop_shell
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition)"
    "find_package(lux-engine-editor-workspace-store REQUIRED COMPONENTS workspace_store)"
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui_rendering input)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS window)")
lux_classify_target(TARGET desktop_shell LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-desktop VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS view_host desktop_shell)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_view_host_test test/host.cpp)
    target_link_libraries(editor_view_host_test PRIVATE view_host editor_composition)
    lux_editor_test_options(editor_view_host_test)
    add_test(NAME editor.view_host COMMAND editor_view_host_test)
    set_tests_properties(editor.view_host PROPERTIES LABELS "editor;native;P10" TIMEOUT 60 RUN_SERIAL TRUE)
endif()

# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WorkspaceActions.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopError.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopShell.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/Presentation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/ViewHost.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/ReviewView.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WindowInput.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WindowOutput.hpp
    DESTINATION include/lux/engine/editor/desktop COMPONENT lux_engine_editor_desktop)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/views/IViewHost.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/views/ViewError.hpp
    DESTINATION include/lux/engine/editor/views COMPONENT lux_engine_editor_view_api)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/views/ViewFactory.hpp
    DESTINATION include/lux/engine/editor/views COMPONENT lux_engine_editor_view_api)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    target_compile_definitions(desktop_shell PRIVATE LUX_DESKTOP_TEST_ACCESS=1)
endif()
