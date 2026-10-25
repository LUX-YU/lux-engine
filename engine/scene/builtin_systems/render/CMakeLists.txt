generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SCENE_RENDER_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SCENE_RENDER_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SCENE_RENDER_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/scene/render/visibility.h
)

add_component(
    COMPONENT_NAME scene_render
    NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_render
    SOURCE_FILES
        src/RenderSystem.cpp
        src/SceneCamera.cpp
        src/RenderSystemConfiguration.cpp
)

component_include_directories(
    scene_render
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_include_directories(scene_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)

target_compile_definitions(scene_render PRIVATE LUX_ENGINE_SCENE_RENDER_LIBRARY)

# Sender instantiations plus ASan instrumentation exceed COFF's normal section count.
target_compile_options(scene_render PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

target_link_libraries(
    scene_render
    PUBLIC
        lux::engine::engine_rendering
        lux::engine::scene::scene_composition
        lux::engine::scene::scene_camera
        lux::engine::simulation::ecs::transform
        lux::engine::simulation::ecs::visual
)

component_add_transitive_commands(
    scene_render
    "find_package(lux-engine-rendering REQUIRED COMPONENTS engine_rendering)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-camera REQUIRED COMPONENTS scene_camera)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS transform visual)"
)

lux_classify_target(TARGET scene_render LAYER SCENE PRODUCT RUNTIME ROLE LIBRARY)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-render
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS scene_render
)

if(BUILD_TESTING)
    add_executable(engine_scene_render_test test/SceneRendering.cpp)
    target_link_libraries(
        engine_scene_render_test
        PRIVATE scene_render lux::engine::toolchain::toolchain_material_compiler
    )
    add_test(NAME engine.scene_render COMMAND engine_scene_render_test)
    set_tests_properties(engine.scene_render PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()
