find_package(lux-cxx REQUIRED COMPONENTS memory)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_WORLD_MATERIALIZATION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_WORLD_MATERIALIZATION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_WORLD_MATERIALIZATION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/world_materialization/visibility.h
)

add_component(
    COMPONENT_NAME scene_world_materialization
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_world_materialization
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldMaterializer.cpp
)
component_include_directories(
    scene_world_materialization
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    scene_world_materialization
    PRIVATE LUX_ENGINE_SCENE_WORLD_MATERIALIZATION_LIBRARY
)
if(MSVC)
    target_compile_options(scene_world_materialization PUBLIC /Zc:__cplusplus /Zc:preprocessor)
endif()
target_link_libraries(
    scene_world_materialization
    PUBLIC
        lux::engine::scene::scene_composition
        lux::engine::world::storage
        lux::engine::simulation::ecs::schema
        lux::cxx::memory
)
component_add_transitive_commands(
    scene_world_materialization
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)"
)

install_components(
    PROJECT_NAME lux-engine-scene-world-materialization
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_world_materialization
)
