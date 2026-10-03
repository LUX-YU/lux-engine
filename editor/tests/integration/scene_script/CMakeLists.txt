if(LUX_EDITOR_BUILD_NATIVE_TESTS AND TARGET scene_script_asset_scene_test)
    add_executable(editor_script_run_test main.cpp)
    target_link_libraries(editor_script_run_test PRIVATE scene_execution scene_script_runtime scene_script_assets_lua)
    get_target_property(_delay_generated simulation_script LUX_SCRIPT_ABILITY_GENERATED_DIR)
    target_include_directories(editor_script_run_test PRIVATE ${_delay_generated}
        ${PROJECT_SOURCE_DIR}/engine/scene/integration/script/test)
    add_dependencies(editor_script_run_test ec2_script_asset_package)
    lux_editor_test_options(editor_script_run_test)
    add_test(NAME editor.script_run COMMAND editor_script_run_test
        ${PROJECT_BINARY_DIR}/engine/scene/integration/script/ec2-assets.lux ${CMAKE_CURRENT_BINARY_DIR}/ec2-run.pak)
    set_tests_properties(editor.script_run PROPERTIES TIMEOUT 45 LABELS "editor;native;EC2")
endif()
