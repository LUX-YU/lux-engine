add_interface_component(COMPONENT_NAME view_api NAMESPACE lux::engine::editor)
component_include_directories(view_api BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(view_api INTERFACE lux::engine::function::ui lux::engine::editor::editor_contracts)
component_add_transitive_commands(view_api
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts)")
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

add_component(COMPONENT_NAME view_host NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/ViewHost.cpp)
component_include_directories(view_host BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(view_host PUBLIC lux::engine::editor::view_api lux::engine::editor::layout_model)
component_add_transitive_commands(view_host
    "find_package(lux-engine-editor-view-api REQUIRED COMPONENTS view_api)"
    "find_package(lux-engine-editor-layout REQUIRED COMPONENTS layout_model)")
lux_classify_target(TARGET view_host LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
add_component(COMPONENT_NAME desktop_shell NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/DesktopShell.cpp src/WindowInput.cpp src/WindowOutput.cpp src/Presentation.cpp src/UiRenderSyncStage.cpp)
component_include_directories(desktop_shell BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
component_add_internal_dependencies(desktop_shell view_host)
target_link_libraries(desktop_shell PUBLIC lux::engine::scene::scene_render
    lux::engine::function::ui_rendering lux::engine::function::input lux::engine::platform::window)
component_add_transitive_commands(desktop_shell
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui_rendering input)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS window)")
lux_classify_target(TARGET desktop_shell LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-desktop VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS view_host desktop_shell)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_view_host_test test/host.cpp)
    target_link_libraries(editor_view_host_test PRIVATE view_host)
    lux_editor_test_options(editor_view_host_test)
    add_test(NAME editor.view_host COMMAND editor_view_host_test)
    set_tests_properties(editor.view_host PROPERTIES LABELS "editor;native;P10" TIMEOUT 60 RUN_SERIAL TRUE)
endif()

# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopError.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopShell.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/Presentation.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/ViewHost.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WindowInput.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/WindowOutput.hpp
    DESTINATION include/lux/engine/editor/desktop COMPONENT lux_engine_editor_desktop)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/views/IViewHost.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/views/ViewError.hpp
    DESTINATION include/lux/engine/editor/views COMPONENT lux_engine_editor_view_api)
