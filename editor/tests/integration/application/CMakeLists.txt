if(LUX_EDITOR_TEST_DESKTOP_GPU AND LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_application_test application.cpp)
    target_compile_definitions(editor_application_test PRIVATE LUX_APPLICATION_TEST_ACCESS=1)
    target_compile_definitions(editor_bootstrap PRIVATE LUX_APPLICATION_TEST_ACCESS=1)
    target_link_libraries(editor_application_test PRIVATE editor_bootstrap project_tools_ui editor_builtin_contributions desktop_shell editor_assets editor_storage
        editor_file_publication editor_persistence_execution workspace_store project_ui tasks_ui
        lux::engine::engine_context_render lux::engine::project::project_plugin_rendering)
    target_include_directories(editor_application_test PRIVATE
        "${CMAKE_SOURCE_DIR}/editor/application/pinclude"
        "${CMAKE_SOURCE_DIR}/editor/workbench/desktop/pinclude")
    lux_editor_test_options(editor_application_test)
    add_test(NAME editor.application COMMAND editor_application_test "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(editor.application PROPERTIES LABELS "editor;P12;gpu" TIMEOUT 180 RUN_SERIAL TRUE)
endif()
