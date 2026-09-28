add_component(COMPONENT_NAME flowforge_model NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/FlowSession.cpp src/edits/FlowGraphEdit.cpp)
component_include_directories(flowforge_model
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flowforge_model PUBLIC lux::engine::editor::edit_sessions lux::engine::editor::edit_history
    lux::engine::function::flowforge)
component_add_transitive_commands(flowforge_model
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions)"
    "find_package(lux-engine-function REQUIRED COMPONENTS flowforge)")
lux_classify_target(TARGET flowforge_model LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge-model VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flowforge_model)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_flowforge_model_test test/flow_session.cpp)
    target_link_libraries(editor_flowforge_model_test PRIVATE flowforge_model)
    lux_editor_test_options(editor_flowforge_model_test)
    foreach(scenario content failure mixed-signature mixed-recreate lifetime reading
        reload-unbound reload-unbound-external reload-reading reload-closing reload-identity reload-success
        input-stale input-reading input-closing budgets ids-undo-branch ids-candidate-seed
        ids-signature ids-restore ids-failure ids-exhaustion)
        add_test(NAME editor.flowforge_model.${scenario} COMMAND editor_flowforge_model_test ${scenario})
        set_tests_properties(editor.flowforge_model.${scenario} PROPERTIES LABELS "editor;native;P04")
    endforeach()
endif()
