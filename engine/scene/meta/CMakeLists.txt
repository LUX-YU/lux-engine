generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_META_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_META_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_META_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/meta/visibility.h
)

add_component(
    COMPONENT_NAME scene_meta
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_meta
    SOURCE_FILES  ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneMetaManager.cpp
)
component_include_directories(
    scene_meta
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(scene_meta PRIVATE LUX_ENGINE_SCENE_META_LIBRARY)
target_link_libraries(
    scene_meta
    PUBLIC
        lux::engine::scene::scene_system
        lux::engine::simulation::system
        lux::engine::simulation::ecs::schema
        lux::engine::function::render_client
        lux::engine::core::meta
        lux::engine::core::serialization
)
component_add_transitive_commands(
    scene_meta
    "find_package(lux-engine-scene-system REQUIRED COMPONENTS scene_system)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta serialization)"
)
lux_classify_target(TARGET scene_meta LAYER SCENE PRODUCT RUNTIME ROLE COMPOSITION)

add_executable(scene_meta_test test/scene_meta_test.cpp)
target_link_libraries(scene_meta_test PRIVATE lux::engine::scene::scene_meta)
target_link_libraries(scene_meta_test PRIVATE render_client_meta)
if(MSVC)
    target_compile_options(scene_meta_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_meta_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET scene_meta_test LAYER SCENE PRODUCT TEST ROLE COMPOSITION)
add_test(NAME scene_meta_test COMMAND scene_meta_test)

install_components(
    PROJECT_NAME lux-engine-scene-meta
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_meta
)
