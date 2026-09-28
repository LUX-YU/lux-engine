find_package(lux-cxx REQUIRED COMPONENTS compile_time container)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDIT_SESSIONS_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDIT_SESSIONS_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDIT_SESSIONS_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/sessions/visibility.h)
add_component(COMPONENT_NAME edit_sessions NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_edit_sessions
    SOURCE_FILES src/PersistenceCheckpoint.cpp src/SessionState.cpp src/SessionStore.cpp)
component_include_directories(edit_sessions
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
target_compile_definitions(edit_sessions PRIVATE LUX_EDIT_SESSIONS_LIBRARY)
target_link_libraries(edit_sessions PUBLIC lux::engine::editor::edit_history lux::engine::editor::editor_contracts
    lux::engine::resource::identity PRIVATE lux::cxx::container)
component_add_transitive_commands(edit_sessions
    "find_package(lux-engine-edit-history REQUIRED COMPONENTS edit_history)"
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)")
lux_classify_target(TARGET edit_sessions LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-edit-sessions VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS edit_sessions)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_sessions_test test/sessions.cpp)
    target_link_libraries(editor_sessions_test PRIVATE edit_sessions)
    lux_editor_test_options(editor_sessions_test)
    add_test(NAME editor.sessions COMMAND editor_sessions_test)
    set_tests_properties(editor.sessions PROPERTIES LABELS "editor;native;P01")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(MSVC)
        add_test(NAME editor.sessions_scope_compile
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/scope_compile.py"
                --build "${CMAKE_BINARY_DIR}")
        set_tests_properties(editor.sessions_scope_compile PROPERTIES LABELS "editor;native;P01")
    endif()
endif()
