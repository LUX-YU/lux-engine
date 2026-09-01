generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/visibility.h
)

add_component(
    COMPONENT_NAME scene_core
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_core
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Scene.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneBuilder.cpp
)

component_include_directories(
    scene_core
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(scene_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
target_compile_definitions(scene_core PRIVATE LUX_ENGINE_SCENE_LIBRARY)
target_link_libraries(
    scene_core
    PUBLIC
        lux::engine::world::description
        lux::engine::simulation::simulation_core
        lux::engine::scene::scene_description
        lux::engine::scene::scene_system
        lux::engine::scene::scene_meta
        lux::engine::resource::identity
)
component_add_transitive_commands(
    scene_core
    "find_package(lux-engine-world-description REQUIRED COMPONENTS world_description)"
    "find_package(lux-engine-simulation-core REQUIRED COMPONENTS simulation_core)"
    "find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)"
    "find_package(lux-engine-scene-system REQUIRED COMPONENTS scene_system)"
    "find_package(lux-engine-scene-meta REQUIRED COMPONENTS scene_meta)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)"
)
lux_classify_target(
    TARGET  scene_core
    LAYER   SCENE
    PRODUCT RUNTIME
    ROLE    COMPOSITION
)

add_executable(scene_core_test test/scene_core_test.cpp)
target_link_libraries(scene_core_test PRIVATE lux::engine::scene::scene_core)
if(MSVC)
    target_compile_options(scene_core_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_core_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  scene_core_test
    LAYER   SCENE
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(NAME scene_core_test COMMAND scene_core_test)

install_components(
    PROJECT_NAME lux-engine-scene
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_core
)
