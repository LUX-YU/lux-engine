generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SCENE_WORLD_LOADING_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SCENE_WORLD_LOADING_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/scene/world_loading/visibility.h)
add_component(COMPONENT_NAME scene_world_loading NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_world_loading SOURCE_FILES src/WorldLoadingSystem.cpp src/Observer.cpp)
include(${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME scene_observer_component_codegen TARGET scene_world_loading
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/Observer.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/Observer.hpp
    LOGICAL_PATH lux/engine/scene/Observer.hpp SYMBOL sceneObserver)
engine_target_add_static_type_info(
    NAME world_loading_configuration_codegen TARGET scene_world_loading
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldLoadingSystem.cpp
    HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/WorldLoadingConfiguration.hpp
    LOGICAL_PATHS lux/engine/scene/WorldLoadingConfiguration.hpp)
component_include_directories(scene_world_loading
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_compile_definitions(scene_world_loading PRIVATE LUX_ENGINE_SCENE_WORLD_LOADING_LIBRARY)
target_link_libraries(scene_world_loading PUBLIC
    lux::engine::scene::scene_world_materialization lux::engine::process::process_world_loading)
component_add_transitive_commands(scene_world_loading
    "find_package(lux-engine-scene-world-materialization REQUIRED COMPONENTS scene_world_materialization)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)")
lux_engine_install_components(PROJECT_NAME lux-engine-scene-world-loading VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene COMPONENTS scene_world_loading)

if(BUILD_TESTING)
    add_executable(scene_world_loading_test test/world_loading.cpp)
    target_link_libraries(scene_world_loading_test PRIVATE lux::engine::scene::scene_world_loading
        lux::engine::simulation::ecs::hierarchy lux::engine::simulation::ecs::transform)
    if(MSVC)
        target_compile_options(scene_world_loading_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_world_loading_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.world_loading COMMAND scene_world_loading_test ${CMAKE_CURRENT_BINARY_DIR}/fixture)
endif()
