add_component(COMPONENT_NAME project_io NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ProjectArtifactStore.cpp src/SaveExecution.cpp)
component_include_directories(project_io
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(project_io PUBLIC lux::engine::editor::editor_persistence lux::engine::process::process_execution
    PRIVATE lux::engine::editor::editor_file_publication)
component_add_transitive_commands(project_io
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)"
    "find_package(lux-engine-editor-file-publication REQUIRED COMPONENTS editor_file_publication)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)")
lux_classify_target(TARGET project_io LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-project-io VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS project_io)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_publication_test test/publication.cpp)
    target_link_libraries(editor_publication_test PRIVATE project_io)
    lux_editor_test_options(editor_publication_test)
    add_test(NAME editor.persistence.real_files COMMAND editor_publication_test "${CMAKE_CURRENT_BINARY_DIR}/test-files")
    set_tests_properties(editor.persistence.real_files PROPERTIES LABELS "editor;native;P05")
endif()
