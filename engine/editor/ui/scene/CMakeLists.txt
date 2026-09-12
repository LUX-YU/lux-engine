generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_SCENE_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_SCENE_UI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_SCENE_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/ui/scene/visibility.h)
add_component(COMPONENT_NAME editor_scene_ui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_scene_ui
    SOURCE_FILES src/SceneWorkspace.cpp src/SceneViewport.cpp src/SceneOutliner.cpp src/SceneInspector.cpp
        src/SceneResourcesPane.cpp src/FirstPartySceneReaders.cpp src/CodegenInput.cpp)
component_include_directories(editor_scene_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_compile_definitions(editor_scene_ui PRIVATE LUX_EDITOR_SCENE_UI_LIBRARY)
target_link_libraries(editor_scene_ui PUBLIC lux::engine::editor::editor_ui lux::engine::editor::editor_scene_session
    PRIVATE lux::engine::simulation::ecs::hierarchy)
component_add_transitive_commands(editor_scene_ui
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)"
    "find_package(lux-engine-editor-scene-session REQUIRED COMPONENTS editor_scene_session)")
lux_classify_target(TARGET editor_scene_ui LAYER EDITOR PRODUCT EDITOR ROLE INTEGRATION)
component_add_cmake_scripts(editor_scene_ui
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/codegen/
    DESTINATION share/lux-engine-editor-scene-ui/editor_scene_ui/cmake_scripts/codegen
    PATTERN "__pycache__" EXCLUDE)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
set(inspector_source ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp)
set(inspector_support ${CMAKE_CURRENT_SOURCE_DIR}/codegen/support/FirstPartyInspectorWidgets.hpp)
engine_target_add_imgui_inspector_codegen(NAME transform_inspectors TARGET editor_scene_ui
    SOURCE_FILE ${inspector_source}
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp
    COMPONENTS lux::simulation::ecs::Transform2D lux::simulation::ecs::Transform3D)
engine_target_add_imgui_inspector_codegen(NAME hierarchy_inspectors TARGET editor_scene_ui
    SOURCE_FILE ${inspector_source}
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp COMPONENTS lux::simulation::ecs::Parent
    CUSTOM_TYPES lux::simulation::ecs::Entity CUSTOM_HEADERS FirstPartyInspectorWidgets.hpp DEPENDS ${inspector_support})
engine_target_add_imgui_inspector_codegen(NAME visual_inspectors TARGET editor_scene_ui
    SOURCE_FILE ${inspector_source}
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/visual/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp
    COMPONENTS lux::simulation::ecs::Mesh3D lux::simulation::ecs::Light3D
    CUSTOM_TYPES lux::rdesc::MeshVisualDescription lux::rdesc::LightDescription
    CUSTOM_HEADERS FirstPartyInspectorWidgets.hpp DEPENDS ${inspector_support})
# The installed generator needs headers/import library matching the already deployed shared ImGui backend.
# Keep this build support under the Editor package; no lower module header or DLL is modified.
get_target_property(inspector_backend_headers imgui::core INTERFACE_INCLUDE_DIRECTORIES)
find_path(inspector_imgui_include imgui.h PATHS ${inspector_backend_headers} NO_DEFAULT_PATH REQUIRED)
install(FILES ${inspector_imgui_include}/imgui.h ${inspector_imgui_include}/imconfig.h
    ${inspector_imgui_include}/imgui_stdlib.h
    DESTINATION share/lux-engine-editor-scene-ui/editor_scene_ui/cmake_scripts/codegen/backend)
install(FILES $<TARGET_LINKER_FILE:imgui::core> DESTINATION lib)
install(FILES $<TARGET_FILE:imgui::core> DESTINATION bin)
install_components(PROJECT_NAME lux-engine-editor-scene-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_scene_ui)

if(BUILD_TESTING)
    add_executable(editor_inspector_benchmark test/InspectorBenchmark.cpp)
    target_link_libraries(editor_inspector_benchmark PRIVATE editor_scene_ui)
    target_include_directories(editor_inspector_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    target_compile_options(editor_inspector_benchmark PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
    lux_classify_target(TARGET editor_inspector_benchmark LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
    engine_target_add_imgui_inspector_codegen(NAME perf_inspectors TARGET editor_inspector_benchmark
        SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/test/InspectorBenchmark.cpp
        HEADER ${CMAKE_CURRENT_SOURCE_DIR}/test/InspectorFixture.hpp LOGICAL_PATH InspectorFixture.hpp
        COMPONENTS inspector_fixture::SmallComponent)
    add_executable(editor_generated_inspector_test test/GeneratedInspectorTest.cpp)
    target_link_libraries(editor_generated_inspector_test PRIVATE editor_scene_ui)
    target_include_directories(editor_generated_inspector_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    target_compile_definitions(editor_generated_inspector_test PRIVATE LUX_EDITOR_INSPECTOR_TEST_PROBE)
    target_compile_options(editor_generated_inspector_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
    lux_classify_target(TARGET editor_generated_inspector_test LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
    engine_target_add_imgui_inspector_codegen(NAME fixture_inspectors TARGET editor_generated_inspector_test
        SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/test/GeneratedInspectorTest.cpp
        HEADER ${CMAKE_CURRENT_SOURCE_DIR}/test/InspectorFixture.hpp LOGICAL_PATH InspectorFixture.hpp
        COMPONENTS inspector_fixture::Component inspector_fixture::SmallComponent
        CUSTOM_TYPES inspector_fixture::Angle CUSTOM_HEADERS CustomInspectorWidget.hpp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/CustomInspectorWidget.hpp)
    add_test(NAME editor_generated_inspector_types COMMAND editor_generated_inspector_test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME editor_generated_inspector_codegen COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test/CodegenContractTest.py
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/inspector_codegen.py
        ${CMAKE_CURRENT_BINARY_DIR}/lux_codegen/fixture_inspectors.inspector.json
        ${CMAKE_CURRENT_BINARY_DIR}/inspector_ir/fixture_inspectors/InspectorFixture.editor-ir.json)
endif()
