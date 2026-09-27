find_package(EnTT CONFIG REQUIRED)
include_component_cmake_scripts(meta)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_RENDER_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_RENDER_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_RENDER_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/render/visibility.h
)
add_component(
    COMPONENT_NAME scene_render
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_render
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Builtin3DRenderStages.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Camera.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CameraExtraction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneRenderSchema.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderAssets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderResources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderResourceAssets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderResourceScenes.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderResourceViews.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderSystemConfiguration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderViewRequests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderSystem.cpp
)
include(${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME scene_camera_component_codegen
    TARGET scene_render
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneRenderSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/Camera.hpp
    LOGICAL_PATH lux/engine/scene/Camera.hpp
    SYMBOL sceneCamera
)
engine_target_add_static_type_info(
    NAME scene_render_configuration_codegen
    TARGET scene_render
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderSystem.cpp
    HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/RenderSystemConfiguration.hpp
    LOGICAL_PATHS lux/engine/scene/RenderSystemConfiguration.hpp
)
component_include_directories(
    scene_render
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(scene_render PRIVATE LUX_ENGINE_SCENE_RENDER_LIBRARY)
target_link_libraries(
    scene_render
    PUBLIC
        lux::engine::function::render_feature_client
        lux::engine::function::render_runtime
        lux::engine::scene::scene_composition
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::transform
        lux::engine::simulation::ecs::visual
        lux::engine::resource::identity
        lux::engine::resource::asset
        lux::engine::scene::scene_mesh_query
        lux::engine::process::asset_loading
    PRIVATE EnTT::EnTT
)
component_add_transitive_commands(
    scene_render
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_feature_client render_runtime)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core transform visual)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity asset)"
    "find_package(lux-engine-scene-mesh-query REQUIRED COMPONENTS scene_mesh_query)"
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-render
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_render
)

if(BUILD_TESTING)
    add_executable(scene_render_resources_test test/resources.cpp test/runtime.cpp)
    target_link_libraries(scene_render_resources_test PRIVATE scene_render lux::engine::function::render_features
        lux::engine::engine_context_render lux::engine::function::ui_rendering)
    target_include_directories(scene_render_resources_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/type_info)

    if(MSVC)
        target_compile_options(scene_render_resources_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(scene_render_resources_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.render_resources COMMAND scene_render_resources_test )
    set_tests_properties(scene.render_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()
