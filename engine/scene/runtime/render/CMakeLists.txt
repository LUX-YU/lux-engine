find_package(EnTT CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_RUNTIME_RENDER_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_RUNTIME_RENDER_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_RUNTIME_RENDER_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/runtime/render/visibility.h
)
add_component(
    COMPONENT_NAME scene_runtime_render
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_runtime_render
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Builtin3DRenderStages.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderSyncPipeline.cpp
)
component_include_directories(
    scene_runtime_render
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(scene_runtime_render PRIVATE LUX_ENGINE_SCENE_RUNTIME_RENDER_LIBRARY)
target_link_libraries(
    scene_runtime_render
    PUBLIC
        lux::engine::function::render_client
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::transform
        lux::engine::simulation::ecs::visual
        lux::engine::resource::identity
    PRIVATE EnTT::EnTT
)
component_add_transitive_commands(
    scene_runtime_render
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core transform visual)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)"
)
lux_classify_target(TARGET scene_runtime_render LAYER SCENE PRODUCT RUNTIME ROLE INTEGRATION)

add_executable(render_sync_pipeline_test test/render_sync_pipeline_test.cpp)
target_link_libraries(render_sync_pipeline_test PRIVATE lux::engine::scene::scene_runtime_render)
if(MSVC)
    target_compile_options(render_sync_pipeline_test PRIVATE /UNDEBUG)
else()
    target_compile_options(render_sync_pipeline_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET render_sync_pipeline_test LAYER SCENE PRODUCT TEST ROLE INTEGRATION)
add_test(NAME render_sync_pipeline_test COMMAND render_sync_pipeline_test)

foreach(render_sync_test IN ITEMS
    render_sync_stage_test
    render_sync_stage_lifecycle_test
    render_sync_pipeline_transaction_test
)
    add_executable(${render_sync_test} test/${render_sync_test}.cpp)
    target_link_libraries(${render_sync_test} PRIVATE lux::engine::scene::scene_runtime_render)
    if(MSVC)
        target_compile_options(${render_sync_test} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${render_sync_test} PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(TARGET ${render_sync_test} LAYER SCENE PRODUCT TEST ROLE INTEGRATION)
    add_test(NAME ${render_sync_test} COMMAND ${render_sync_test})
endforeach()
set_tests_properties(render_sync_stage_lifecycle_test PROPERTIES TIMEOUT 30)

install_components(
    PROJECT_NAME lux-engine-scene-render
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_runtime_render
)
