add_component(COMPONENT_NAME flowforge_model_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/PreparedFlowReload.cpp src/FlowSession.cpp src/FlowPersistenceAccess.cpp src/FlowEnvironment.cpp src/edits/FlowGraphEdit.cpp)
component_include_directories(flowforge_model_legacy
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flowforge_model_legacy PUBLIC lux::engine::editor::edit_sessions_legacy lux::engine::editor::edit_history_legacy
    lux::engine::function::flowforge)
component_add_transitive_commands(flowforge_model_legacy
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions_legacy)"
    "find_package(lux-engine-function REQUIRED COMPONENTS flowforge)")
lux_classify_target(TARGET flowforge_model_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge-model VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flowforge_model_legacy)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_flowforge_model_test_legacy test/flow_session.cpp)
    target_link_libraries(editor_flowforge_model_test_legacy PRIVATE flowforge_model_legacy)
    lux_editor_test_options(editor_flowforge_model_test_legacy)
    foreach(scenario content failure mixed-signature mixed-recreate lifetime reading
        reload-unbound reload-unbound-external reload-reading reload-closing reload-identity reload-success
        input-stale input-reading input-closing budgets ids-undo-branch ids-candidate-seed
        ids-signature ids-restore ids-failure ids-exhaustion)
        add_test(NAME editor.flowforge_model_legacy.${scenario} COMMAND editor_flowforge_model_test_legacy ${scenario})
        set_tests_properties(editor.flowforge_model_legacy.${scenario} PROPERTIES LABELS "editor;native;P04")
    endforeach()
endif()
