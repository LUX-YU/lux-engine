if(LUX_EDITOR_TEST_DESKTOP_GPU AND LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_application_test application.cpp)
    target_compile_definitions(editor_application_test PRIVATE LUX_APPLICATION_TEST_ACCESS=1)
    target_compile_definitions(editor_bootstrap PRIVATE LUX_APPLICATION_TEST_ACCESS=1)
    target_link_libraries(editor_application_test PRIVATE scene_ui material_ui flow_ui scene_persistence material_persistence flowforge_persistence editor_bootstrap project_tools_ui editor_extensions desktop_shell editor_assets editor_storage
        editor_file_publication editor_persistence_execution workspace_store project_ui tasks_ui
        lux::engine::engine_context_render lux::engine::project::project_plugin_rendering)
    target_include_directories(editor_application_test PRIVATE
        "${CMAKE_SOURCE_DIR}/editor/application/pinclude"
        "${CMAKE_SOURCE_DIR}/editor/workbench/desktop/pinclude")
    lux_editor_test_options(editor_application_test)
    add_test(NAME editor.application COMMAND editor_application_test "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(editor.application PROPERTIES LABELS "editor;P12;gpu" TIMEOUT 180 RUN_SERIAL TRUE)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_settings_content_test settings.cpp)
    target_link_libraries(editor_settings_content_test PRIVATE project_tools_ui editor_extensions editor_file_publication)
    lux_editor_test_options(editor_settings_content_test)
    add_test(NAME editor.settings.content COMMAND editor_settings_content_test "${CMAKE_CURRENT_BINARY_DIR}/settings")
    set_tests_properties(editor.settings.content PROPERTIES LABELS "editor;native;EC3" TIMEOUT 60 RUN_SERIAL TRUE)
endif()
