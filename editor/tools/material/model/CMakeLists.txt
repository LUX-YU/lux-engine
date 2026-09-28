add_component(COMPONENT_NAME material_model NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/MaterialSession.cpp src/edits/MaterialGraphEdit.cpp src/edits/MaterialValueEdit.cpp)
component_include_directories(material_model
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_model PUBLIC lux::engine::editor::edit_sessions lux::engine::editor::edit_history
    lux::engine::function::material_graph)
component_add_transitive_commands(material_model
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions)"
    "find_package(lux-engine-function REQUIRED COMPONENTS material_graph)")
lux_classify_target(TARGET material_model LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-model VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_model)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_material_model_test test/material_session.cpp)
    target_link_libraries(editor_material_model_test PRIVATE material_model)
    lux_editor_test_options(editor_material_model_test)
    foreach(scenario content failure layout lifetime mixed-replace mixed-recreate reading
        reload-unbound reload-unbound-lease reload-closing-lease reload-reading-lease reload-outcomes)
        add_test(NAME editor.material_model.${scenario} COMMAND editor_material_model_test ${scenario})
        set_tests_properties(editor.material_model.${scenario} PROPERTIES LABELS "editor;native;P03")
    endforeach()
endif()
