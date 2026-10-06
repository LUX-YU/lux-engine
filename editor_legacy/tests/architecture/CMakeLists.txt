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
        --compile-db "${CMAKE_BINARY_DIR}/compile_commands.json" --stage "${LUX_EDITOR_MIGRATION_STAGE}"
        --layering-mode "${LUX_EDITOR_LAYERING_MODE}")
set_tests_properties(editor.architecture_current PROPERTIES LABELS "editor;native;architecture")
add_executable(editor_flow_source_native_legacy flow_source_native.cpp)
target_link_libraries(editor_flow_source_native_legacy PRIVATE lux::engine::function::flowforge)
lux_editor_test_options(editor_flow_source_native_legacy)
add_test(NAME editor.flow_source_native COMMAND editor_flow_source_native_legacy)
set_tests_properties(editor.flow_source_native PROPERTIES LABELS "editor;native")

add_test(NAME editor.material_model_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_scene_model_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}" --model material)
set_tests_properties(editor.material_model_boundaries PROPERTIES LABELS "editor;native;P03" TIMEOUT 120)

add_test(NAME editor.flowforge_model_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_scene_model_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}" --model flowforge)
set_tests_properties(editor.flowforge_model_boundaries PROPERTIES LABELS "editor;native;P04" TIMEOUT 120)
add_executable(editor_three_sessions_test_legacy three_sessions.cpp)
target_link_libraries(editor_three_sessions_test_legacy PRIVATE scene_model_legacy material_model_legacy flowforge_model_legacy)
lux_editor_test_options(editor_three_sessions_test_legacy)
add_test(NAME editor.three_actual_sessions COMMAND editor_three_sessions_test_legacy)
set_tests_properties(editor.three_actual_sessions PROPERTIES LABELS "editor;native;P04")
add_test(NAME editor.persistence_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_persistence_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/persistence-boundaries.json")
set_tests_properties(editor.persistence_boundaries PROPERTIES LABELS "editor;native;P05" TIMEOUT 180)
add_test(NAME editor.scene_execution_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_scene_execution_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/scene-execution-boundaries.json")
set_tests_properties(editor.scene_execution_boundaries PROPERTIES LABELS "editor;native;P06" TIMEOUT 180)

add_test(NAME editor.projection_compilation_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_projection_compilation_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/projection-compilation-boundaries.json")
set_tests_properties(editor.projection_compilation_boundaries PROPERTIES LABELS "editor;native;P07" TIMEOUT 180)

add_test(NAME editor.interaction_view_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_interaction_view_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/interaction-view-boundaries.json")
set_tests_properties(editor.interaction_view_boundaries PROPERTIES LABELS "editor;native;P08" TIMEOUT 180)

add_test(NAME editor.workspace_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_workspace_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/workspace-boundaries.json")
set_tests_properties(editor.workspace_boundaries PROPERTIES LABELS "editor;native;P09" TIMEOUT 180)

add_test(NAME editor.desktop_view_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_interaction_view_boundaries.py"
        --stage P10 --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/desktop-view-boundaries.json")
set_tests_properties(editor.desktop_view_boundaries PROPERTIES LABELS "editor;native;P10" TIMEOUT 240)

add_executable(editor_quality_test_legacy quality.cpp)
target_link_libraries(editor_quality_test_legacy PRIVATE editor_persistence_legacy lux::cxx::container)
lux_editor_test_options(editor_quality_test_legacy)
add_test(NAME editor.quality.containers COMMAND editor_quality_test_legacy)
set_tests_properties(editor.quality.containers PROPERTIES LABELS "editor;native;P10Q")
add_test(NAME editor.quality.boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_interaction_view_boundaries.py"
        --stage P10Q --source "${PROJECT_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/quality-boundaries.json")
set_tests_properties(editor.quality.boundaries PROPERTIES LABELS "editor;native;P10Q" TIMEOUT 300)

add_test(NAME editor.quality.evidence
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_quality_evidence.py")
set_tests_properties(editor.quality.evidence PROPERTIES LABELS "editor;native;P10Q")

if(MSVC)
    add_test(NAME editor.layering.delivery_constraints
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_delivery_constraints.py"
            --build "${CMAKE_BINARY_DIR}")
    set_tests_properties(editor.layering.delivery_constraints PROPERTIES LABELS "editor;native;editor_layering" TIMEOUT 120)
endif()

add_test(NAME editor.layering.boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_layering_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --cmake "${CMAKE_COMMAND}")
set_tests_properties(editor.layering.boundaries PROPERTIES LABELS "editor;native;editor_layering" TIMEOUT 240)
if(MSVC)
    add_test(NAME editor.layering.compiler_providers
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_layering_compiler.py"
            --source "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}")
    set_tests_properties(editor.layering.compiler_providers PROPERTIES LABELS "editor;native;editor_layering" TIMEOUT 240)
endif()

add_test(NAME editor.extensions.boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_layering_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --cmake "${CMAKE_COMMAND}" --p11)
set_tests_properties(editor.extensions.boundaries PROPERTIES LABELS "editor;native;P11" TIMEOUT 180)

add_test(NAME editor.product.boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_layering_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --cmake "${CMAKE_COMMAND}" --p12)
set_tests_properties(editor.product.boundaries PROPERTIES LABELS "editor;native;P12" TIMEOUT 180)

add_test(NAME editor.ec2.boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_layering_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --cmake "${CMAKE_COMMAND}" --ec2)
set_tests_properties(editor.ec2.boundaries PROPERTIES LABELS "editor;native;EC2" TIMEOUT 240)

add_test(NAME editor.ec3.boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_layering_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --cmake "${CMAKE_COMMAND}" --ec3)
set_tests_properties(editor.ec3.boundaries PROPERTIES LABELS "editor;native;EC3" TIMEOUT 180)

add_test(NAME editor.ec4.boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_layering_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --cmake "${CMAKE_COMMAND}" --ec4)
set_tests_properties(editor.ec4.boundaries PROPERTIES LABELS "editor;native;EC4" TIMEOUT 180)
