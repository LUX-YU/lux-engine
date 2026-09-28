find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME editor.scene_model_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_scene_model_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}")
set_tests_properties(editor.scene_model_boundaries PROPERTIES LABELS "editor;native;P02" TIMEOUT 120)
add_test(NAME editor.architecture_negative
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_editor_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}")
set_tests_properties(editor.architecture_negative PROPERTIES LABELS "editor;native;architecture" TIMEOUT 120)
add_test(NAME editor.architecture_current
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_editor_boundaries.py"
        --repo "${PROJECT_SOURCE_DIR}" --graph "${CMAKE_BINARY_DIR}/editor-architecture/targets.json"
        --compile-db "${CMAKE_BINARY_DIR}/compile_commands.json" --stage "${LUX_EDITOR_MIGRATION_STAGE}")
set_tests_properties(editor.architecture_current PROPERTIES LABELS "editor;native;architecture")
add_executable(editor_flow_source_native flow_source_native.cpp)
target_link_libraries(editor_flow_source_native PRIVATE lux::engine::function::flowforge)
lux_editor_test_options(editor_flow_source_native)
add_test(NAME editor.flow_source_native COMMAND editor_flow_source_native)
set_tests_properties(editor.flow_source_native PROPERTIES LABELS "editor;native")

add_test(NAME editor.material_model_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_scene_model_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}" --model material)
set_tests_properties(editor.material_model_boundaries PROPERTIES LABELS "editor;native;P03" TIMEOUT 120)
