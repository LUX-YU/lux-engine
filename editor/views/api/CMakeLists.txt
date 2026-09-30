add_interface_component(COMPONENT_NAME view_api NAMESPACE lux::engine::editor)
component_include_directories(view_api BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
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
