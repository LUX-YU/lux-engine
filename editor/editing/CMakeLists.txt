add_interface_component(COMPONENT_NAME editor_contracts NAMESPACE lux::engine::editor)
component_include_directories(editor_contracts
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
find_package(lux-cxx REQUIRED COMPONENTS core compile_time)
target_link_libraries(editor_contracts INTERFACE lux::cxx::core lux::cxx::compile_time)
component_add_transitive_commands(editor_contracts "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)")
lux_classify_target(TARGET editor_contracts LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-contracts VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_contracts)

find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDIT_HISTORY_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDIT_HISTORY_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDIT_HISTORY_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/editing/history_visibility.h
)
add_component(
    COMPONENT_NAME edit_history
    NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_edit_history
    SOURCE_FILES src/history/EditTypes.cpp src/history/EditHistory.cpp
)
component_include_directories(
    edit_history
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(edit_history PRIVATE LUX_EDIT_HISTORY_LIBRARY)
lux_classify_target(TARGET edit_history LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
target_link_libraries(edit_history PUBLIC lux::cxx::compile_time lux::engine::editor::editor_contracts)
component_add_transitive_commands(edit_history "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts)")
component_add_transitive_commands(edit_history "find_package(lux-cxx REQUIRED COMPONENTS compile_time)")

lux_engine_install_components(
    PROJECT_NAME lux-engine-edit-history
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor
    COMPONENTS edit_history
)

find_package(lux-cxx REQUIRED COMPONENTS compile_time container)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDIT_SESSIONS_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDIT_SESSIONS_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDIT_SESSIONS_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/sessions/visibility.h)
add_component(COMPONENT_NAME edit_sessions NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_edit_sessions
    SOURCE_FILES src/sessions/PersistenceCheckpoint.cpp src/sessions/SessionState.cpp src/sessions/SessionStore.cpp)
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
    add_executable(editor_sessions_test test/sessions/sessions.cpp)
    target_link_libraries(editor_sessions_test PRIVATE edit_sessions)
    lux_editor_test_options(editor_sessions_test)
    add_test(NAME editor.sessions COMMAND editor_sessions_test)
    add_test(NAME editor.sessions_close_throw COMMAND editor_sessions_test close-throw)
    add_test(NAME editor.sessions_close_contract COMMAND editor_sessions_test close-contract)
    set_tests_properties(editor.sessions_close_throw editor.sessions_close_contract PROPERTIES LABELS "editor;native;P01-R1")
    set_tests_properties(editor.sessions PROPERTIES LABELS "editor;native;P01")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(MSVC)
        add_test(NAME editor.sessions_scope_compile
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/sessions/scope_compile.py"
                --build "${CMAKE_BINARY_DIR}")
        set_tests_properties(editor.sessions_scope_compile PROPERTIES LABELS "editor;native;P01")
    endif()
endif()

# Existing product adaptation only; removed with its consumers at P12.

find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_EDITING_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_EDITING_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_EDITING_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/editing/visibility.h
)
add_component(
    COMPONENT_NAME editor_editing
    NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_editing
    SOURCE_FILES src/EditHistoryTarget.cpp ../transition/LegacyPersistenceState.cpp
)
component_include_directories(
    editor_editing
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(editor_editing PRIVATE LUX_EDITOR_EDITING_LIBRARY)
lux_classify_target(TARGET editor_editing LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
target_link_libraries(editor_editing PUBLIC lux::cxx::compile_time lux::engine::resource::identity lux::engine::editor::edit_sessions)
component_add_transitive_commands(editor_editing "find_package(lux-engine-resource REQUIRED COMPONENTS identity)")
component_add_transitive_commands(editor_editing "find_package(lux-cxx REQUIRED COMPONENTS compile_time)")

component_add_transitive_commands(editor_editing "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions)")

lux_engine_install_components(
    PROJECT_NAME lux-engine-editor-editing
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor
    COMPONENTS editor_editing
)

