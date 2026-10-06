if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_session_installation_test_legacy installation.cpp)
    target_link_libraries(editor_session_installation_test_legacy PRIVATE
        scene_persistence_legacy material_persistence_legacy flowforge_persistence_legacy
        editor_persistence_execution_legacy editor_file_publication_legacy editor_extensions_legacy desktop_shell_legacy session_execution_legacy)
    lux_editor_test_options(editor_session_installation_test_legacy)
    add_test(NAME editor.sessions.installation
        COMMAND editor_session_installation_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/sources")
    set_tests_properties(editor.sessions.installation PROPERTIES LABELS "editor;native;P11" TIMEOUT 120)
    add_test(NAME editor.sessions.content_operations
        COMMAND editor_session_installation_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/sources" p12)
    set_tests_properties(editor.sessions.content_operations PROPERTIES LABELS "editor;native;P12" TIMEOUT 120)
endif()
