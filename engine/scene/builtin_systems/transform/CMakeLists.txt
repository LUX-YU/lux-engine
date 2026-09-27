generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_TRANSFORM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_TRANSFORM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_TRANSFORM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/transform/visibility.h
)

include_component_cmake_scripts(meta)

add_component(
    COMPONENT_NAME scene_transform
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_transform
    SOURCE_FILES src/TransformSystem.cpp
)

engine_target_add_static_type_info(
    NAME          scene_transform_configuration_codegen
    TARGET        scene_transform
    SOURCE_FILE   ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformSystem.cpp
    HEADERS       ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/TransformSystem.hpp
    LOGICAL_PATHS lux/engine/scene/TransformSystem.hpp
)

component_include_directories(
    scene_transform
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/sinclude
    INSTALL_TIME include
)

target_compile_definitions(
    scene_transform
    PRIVATE LUX_ENGINE_SCENE_TRANSFORM_LIBRARY
)

target_link_libraries(
    scene_transform
    PUBLIC
        lux::engine::scene::scene_composition
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::hierarchy
        lux::engine::simulation::ecs::transform
)

component_add_transitive_commands(
    scene_transform
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core hierarchy transform)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-transform
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS scene_transform
)
