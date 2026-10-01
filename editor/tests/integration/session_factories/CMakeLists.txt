if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_session_installation_test installation.cpp)
    target_link_libraries(editor_session_installation_test PRIVATE
        scene_persistence material_persistence flowforge_persistence
        editor_persistence_execution editor_file_publication editor_extensions desktop_shell)
    lux_editor_test_options(editor_session_installation_test)
    add_test(NAME editor.sessions.installation
        COMMAND editor_session_installation_test "${CMAKE_CURRENT_BINARY_DIR}/sources")
    set_tests_properties(editor.sessions.installation PROPERTIES LABELS "editor;native;P11" TIMEOUT 120)
endif()
