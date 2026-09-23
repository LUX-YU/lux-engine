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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderAssetRequest.cpp
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

if(LUX_PROFILE_HAS_EDITOR)
add_component(
    COMPONENT_NAME scene_render_meta
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_render_meta
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderSystemMetaModule.cpp
)
component_include_directories(
    scene_render_meta
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
lux_classify_target(TARGET scene_render_meta LAYER EDITOR PRODUCT EDITOR ROLE PLUGIN)
target_compile_definitions(scene_render_meta PRIVATE LUX_ENGINE_SCENE_RENDER_LIBRARY)
target_link_libraries(
    scene_render_meta
    PRIVATE scene_render lux::engine::core::meta lux::engine::editor::editor_metadata
)

engine_add_type_runtime_info(
    NAME scene_runtime_render_configuration_meta EXPLICIT_REGISTRATION
    REGISTER_FUNC_NAME scene_runtime_render_configuration
    REGISTER_FUNC_MACRO LUX_ENGINE_SCENE_RENDER_PUBLIC
    CUSTOM_INCLUDE "lux/engine/scene/render/visibility.h"
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/RenderSystemConfiguration.hpp
)
engine_target_add_type_runtime_info(
    TARGET scene_render_meta
    METAS scene_runtime_render_configuration_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/include
    TOP_LEVEL_REGISTER_FUNC_PREFIX LuxRegisterSceneRuntimeRenderMetas
)

endif()
set(_render_components scene_render)
if(TARGET scene_render_meta)
    list(APPEND _render_components scene_render_meta)
endif()
lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-render
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   ${_render_components}
)

if(BUILD_TESTING)
    add_executable(scene_camera_test test/camera.cpp)
    target_link_libraries(scene_camera_test PRIVATE scene_render lux::engine::core::math)
    if(MSVC)
        target_compile_options(scene_camera_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_camera_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.camera COMMAND scene_camera_test)
    add_executable(scene_render_views_test test/system_views.cpp)
    target_link_libraries(scene_render_views_test PRIVATE scene_render
        lux::engine::function::render_features
        lux::engine::function::render_vulkan)
    if(MSVC)
        target_compile_options(scene_render_views_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_render_views_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.render_views COMMAND scene_render_views_test)
endif()

if(BUILD_TESTING AND LUX_PROFILE_HAS_EDITOR)
    add_executable(scene_render_assets_test test/assets.cpp)
    target_link_libraries(scene_render_assets_test PRIVATE scene_render
        lux::engine::function::render_features)
    if(MSVC)
        target_compile_options(scene_render_assets_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_render_assets_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.render_assets_seed COMMAND lux_scene_seed ${CMAKE_CURRENT_BINARY_DIR}/assets.luxpak)
    set_tests_properties(scene.render_assets_seed PROPERTIES FIXTURES_SETUP scene_render_assets)
    add_test(NAME scene.render_assets COMMAND scene_render_assets_test ${CMAKE_CURRENT_BINARY_DIR}/assets.luxpak)
    set_tests_properties(scene.render_assets PROPERTIES FIXTURES_REQUIRED scene_render_assets)
endif()
