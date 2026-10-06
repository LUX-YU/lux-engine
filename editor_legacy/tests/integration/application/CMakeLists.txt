if(LUX_EDITOR_TEST_DESKTOP_GPU AND LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_application_test_legacy application.cpp)
    target_compile_definitions(editor_application_test_legacy PRIVATE LUX_APPLICATION_TEST_ACCESS=1)
    target_compile_definitions(editor_bootstrap_legacy PRIVATE LUX_APPLICATION_TEST_ACCESS=1)
    target_link_libraries(editor_application_test_legacy PRIVATE project_module_legacy scene_ui_legacy material_ui_legacy flow_ui_legacy flow_module_legacy material_module_legacy scene_module_legacy scene_persistence_legacy material_persistence_legacy flowforge_persistence_legacy editor_bootstrap_legacy project_tools_ui_legacy editor_extensions_legacy desktop_shell_legacy editor_assets_legacy editor_storage_legacy
        editor_file_publication_legacy editor_persistence_execution_legacy workspace_store_legacy project_ui_legacy tasks_ui_legacy
        lux::engine::engine_context_render lux::engine::project::project_plugin_rendering)
    target_include_directories(editor_application_test_legacy PRIVATE
        "${CMAKE_SOURCE_DIR}/editor_legacy/application/pinclude"
        "${CMAKE_SOURCE_DIR}/editor_legacy/workbench/desktop/pinclude")
    lux_editor_test_options(editor_application_test_legacy)
    add_test(NAME editor.application COMMAND editor_application_test_legacy "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(editor.application PROPERTIES LABELS "editor;P12;gpu" TIMEOUT 180 RUN_SERIAL TRUE)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_settings_navigation_test_legacy settings_navigation.cpp)
    target_link_libraries(editor_settings_navigation_test_legacy PRIVATE project_tools_ui_legacy editor_extensions_legacy editor_file_publication_legacy)
    lux_editor_test_options(editor_settings_navigation_test_legacy)
    foreach(mode scope readonly catalog)
        add_test(NAME editor.settings.navigation.${mode}
            COMMAND editor_settings_navigation_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/navigation" ${mode})
        set_tests_properties(editor.settings.navigation.${mode} PROPERTIES LABELS "editor;native;EC3" TIMEOUT 60 RUN_SERIAL TRUE)
    endforeach()
    add_executable(editor_settings_content_test_legacy settings.cpp)
    target_link_libraries(editor_settings_content_test_legacy PRIVATE project_tools_ui_legacy editor_extensions_legacy editor_file_publication_legacy)
    lux_editor_test_options(editor_settings_content_test_legacy)
    add_test(NAME editor.settings.content COMMAND editor_settings_content_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/settings")
    set_tests_properties(editor.settings.content PROPERTIES LABELS "editor;native;EC3" TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(LUX_EDITOR_TEST_DESKTOP_GPU AND TARGET editor_settings_content_test_legacy)
    add_test(NAME editor.settings.window COMMAND editor_settings_content_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/settings-window" --window)
    set_tests_properties(editor.settings.window PROPERTIES LABELS "editor;desktop;EC3" TIMEOUT 90 RUN_SERIAL TRUE)
endif()
