find_package(lux-cxx REQUIRED COMPONENTS memory)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_WORLD_RUNTIME_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_WORLD_RUNTIME_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_WORLD_RUNTIME_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/runtime/world/visibility.h
)

add_component(
    COMPONENT_NAME scene_runtime_world
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_runtime_world
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldMaterializer.cpp
)
component_include_directories(
    scene_runtime_world
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    scene_runtime_world
    PRIVATE LUX_ENGINE_SCENE_WORLD_RUNTIME_LIBRARY
)
if(MSVC)
    target_compile_options(scene_runtime_world PUBLIC /Zc:__cplusplus /Zc:preprocessor)
endif()
target_link_libraries(
    scene_runtime_world
    PUBLIC
        lux::engine::scene::scene_core
        lux::engine::world::storage
        lux::engine::simulation::ecs::schema
        lux::cxx::memory
)
component_add_transitive_commands(
    scene_runtime_world
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_core)"
    "find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)"
)
lux_classify_target(
    TARGET  scene_runtime_world
    LAYER   SCENE
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

add_executable(scene_world_runtime_test test/scene_world_runtime_test.cpp)
target_link_libraries(
    scene_world_runtime_test
    PRIVATE
        lux::engine::scene::scene_runtime_world
        lux::engine::process::process_world
        lux::engine::simulation::ecs::transform
)
if(MSVC)
    target_compile_options(scene_world_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_world_runtime_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  scene_world_runtime_test
    LAYER   SCENE
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME scene_world_runtime_test COMMAND scene_world_runtime_test)

add_executable(
    scene_world_runtime_deferred_test
    test/scene_world_runtime_deferred_test.cpp
)
target_link_libraries(
    scene_world_runtime_deferred_test
    PRIVATE
        lux::engine::scene::scene_runtime_world
        lux::engine::process::process_world
        lux::engine::simulation::simulation_core
)
if(MSVC)
    target_compile_options(scene_world_runtime_deferred_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_world_runtime_deferred_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  scene_world_runtime_deferred_test
    LAYER   SCENE
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(
    NAME scene_world_runtime_deferred_test
    COMMAND scene_world_runtime_deferred_test
)

install_components(
    PROJECT_NAME lux-engine-scene-world-runtime
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_runtime_world
)
