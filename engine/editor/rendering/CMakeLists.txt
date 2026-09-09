generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_RENDERING_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_RENDERING_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_RENDERING_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/rendering/visibility.h
)
add_component(COMPONENT_NAME editor_rendering NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_rendering
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EditorRenderer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderView.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ViewResources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ViewImage.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/EditorRenderServer.cpp
)
component_include_directories(editor_rendering
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(editor_rendering PRIVATE LUX_EDITOR_RENDERING_LIBRARY
    LUX_EDITOR_PACKED_CONTENT=$<BOOL:${LUX_BUILD_PACKED_RENDER_CONTENT}>
    $<$<BOOL:${LUX_EDITOR_DIAGNOSTICS}>:LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS>)
target_link_libraries(editor_rendering
    PUBLIC lux::engine::scene::scene_render lux::engine::function::ui
    PRIVATE lux::engine::function::render_vulkan lux::engine::function::render_features
        lux::engine::function::ui_vulkan_backend lux::engine::platform::window
)
component_add_transitive_commands(editor_rendering
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
)
lux_classify_target(TARGET editor_rendering LAYER EDITOR PRODUCT EDITOR ROLE INTEGRATION)
if(LUX_EDITOR_DIAGNOSTICS)
    target_sources(editor_rendering PRIVATE ${PROJECT_SOURCE_DIR}/cmake/diagnostics/EditorAllocationDiagnostics.cpp)
    target_compile_definitions(editor_rendering PRIVATE
        LUX_ALLOCATION_ARM=lux_er1_renderer_allocation_fail_after
        LUX_ALLOCATION_DISARM=lux_er1_renderer_allocation_disarm)
    target_sources(render_client PRIVATE
        ${PROJECT_SOURCE_DIR}/cmake/diagnostics/EditorAllocationDiagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/test/ClientAllocationDiagnostics.cpp)
    target_compile_definitions(render_client PRIVATE
        LUX_ALLOCATION_ARM=lux_er1_client_allocation_fail_after
        LUX_ALLOCATION_DISARM=lux_er1_client_allocation_disarm)
    add_executable(editor_client_allocation_test test/ClientAllocationTest.cpp)
    target_link_libraries(editor_client_allocation_test PRIVATE lux::engine::function::render_client)
    lux_classify_target(TARGET editor_client_allocation_test LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
    add_test(NAME editor_client_allocation_protocol COMMAND editor_client_allocation_test)
endif()
install_components(PROJECT_NAME lux-engine-editor-rendering VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_rendering)
if(BUILD_TESTING)
    add_executable(editor_foreign_renderer_test test/ForeignRendererTest.cpp)
    target_include_directories(editor_foreign_renderer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
    target_link_libraries(editor_foreign_renderer_test PRIVATE lux::engine::editor::editor_rendering
        lux::engine::editor::editor_ui lux::engine::platform::window)
    target_compile_options(editor_foreign_renderer_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
    lux_classify_target(TARGET editor_foreign_renderer_test LAYER EDITOR PRODUCT TEST ROLE COMPOSITION)
    add_executable(editor_barrier_program_test test/BarrierProgramTest.cpp)
    target_link_libraries(editor_barrier_program_test PRIVATE lux::engine::function::render_vulkan)
    target_include_directories(editor_barrier_program_test PRIVATE
        ${PROJECT_SOURCE_DIR}/modules/function/render/vulkan/pinclude)
    target_compile_options(editor_barrier_program_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
    lux_classify_target(TARGET editor_barrier_program_test LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
    add_test(NAME editor_barrier_program_protocol COMMAND editor_barrier_program_test)
    add_executable(editor_reply_budget_test test/ReplyBudgetTest.cpp)
    target_link_libraries(editor_reply_budget_test PRIVATE lux::engine::function::render_client)
    target_include_directories(editor_reply_budget_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
    target_compile_options(editor_reply_budget_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
    lux_classify_target(TARGET editor_reply_budget_test LAYER EDITOR PRODUCT TEST ROLE INTEGRATION)
    add_test(NAME editor_reply_budget_protocol COMMAND editor_reply_budget_test)
endif()
