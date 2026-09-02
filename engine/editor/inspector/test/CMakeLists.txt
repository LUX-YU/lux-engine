add_executable(
    editor_component_binding_test
    component_editor_binding_test.cpp
    EditorHiddenCodegenInput.cpp
    PluginBinding.cpp
    PluginCodegenInput.cpp
)
target_include_directories(editor_component_binding_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(editor_component_binding_test PRIVATE lux::engine::editor::editor_inspector)
engine_target_add_component_editor_codegen(
    NAME plugin_component_editor_codegen
    TARGET editor_component_binding_test
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/PluginCodegenInput.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/PluginComponent.hpp
    LOGICAL_PATH test/PluginComponent.hpp
    SYMBOL plugin
)
engine_target_add_component_editor_codegen(
    NAME hidden_component_editor_codegen
    TARGET editor_component_binding_test
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/EditorHiddenCodegenInput.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/EditorHiddenComponent.hpp
    LOGICAL_PATH test/EditorHiddenComponent.hpp
    SYMBOL hidden
)
if(MSVC)
    target_compile_options(editor_component_binding_test PRIVATE /UNDEBUG)
else()
    target_compile_options(editor_component_binding_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  editor_component_binding_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME editor_component_binding COMMAND editor_component_binding_test)
set_tests_properties(editor_component_binding PROPERTIES LABELS "editor;l5;inspector;codegen")

add_library(editor_invalid_widget_negative STATIC EXCLUDE_FROM_ALL invalid_component_editor_widget.cpp)
target_include_directories(editor_invalid_widget_negative PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(editor_invalid_widget_negative PRIVATE lux::engine::editor::editor_inspector)
engine_target_add_component_editor_codegen(
    NAME invalid_component_editor_widget_codegen
    TARGET editor_invalid_widget_negative
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/invalid_component_editor_widget.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/invalid_component_editor_widget.hpp
    LOGICAL_PATH test/invalid_component_editor_widget.hpp
    SYMBOL invalid_widget
)
lux_classify_target(
    TARGET  editor_invalid_widget_negative
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(
    NAME editor_invalid_widget_rejected
    COMMAND
        ${CMAKE_COMMAND}
        -DBUILD_DIR=${CMAKE_BINARY_DIR}
        -DTARGET=editor_invalid_widget_negative
        -DEXPECTED_PATTERN=unsupported.generated.Editor.widget
        -P ${PROJECT_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
)
set_tests_properties(editor_invalid_widget_rejected PROPERTIES LABELS "editor;l5;inspector;codegen;negative")

add_test(
    NAME editor_generated_api_is_backend_neutral
    COMMAND
        ${CMAKE_COMMAND}
        "-DFILES=${CMAKE_CURRENT_BINARY_DIR}/../component_editor_gen/lux/engine/simulation/ecs/Transform.component_editor.hpp;${CMAKE_CURRENT_BINARY_DIR}/../component_editor_gen/lux/engine/simulation/ecs/Parent.component_editor.hpp;${CMAKE_CURRENT_BINARY_DIR}/../component_editor_gen/lux/engine/simulation/ecs/Visual.component_editor.hpp;${CMAKE_CURRENT_BINARY_DIR}/component_editor_gen/test/PluginComponent.component_editor.hpp;${CMAKE_CURRENT_BINARY_DIR}/component_editor_gen/test/EditorHiddenComponent.component_editor.hpp"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/ValidateInspectorGeneratedApi.cmake
)
set_tests_properties(
    editor_generated_api_is_backend_neutral
    PROPERTIES LABELS "editor;l5;inspector;codegen;architecture"
)

add_executable(editor_entity_inspector_test entity_inspector_test.cpp)
target_include_directories(editor_entity_inspector_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(editor_entity_inspector_test PRIVATE lux::engine::editor::editor_inspector)
if(MSVC)
    target_compile_options(editor_entity_inspector_test PRIVATE /UNDEBUG)
else()
    target_compile_options(editor_entity_inspector_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  editor_entity_inspector_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME editor_entity_inspector COMMAND editor_entity_inspector_test)
set_tests_properties(editor_entity_inspector PROPERTIES LABELS "editor;l5;inspector;selection;undo")
