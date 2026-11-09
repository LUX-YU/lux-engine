generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_TRANSFORM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_TRANSFORM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_TRANSFORM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/transform/visibility.h
)

include_component_cmake_scripts(meta)

add_component(
    COMPONENT_NAME system_transform
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_system_transform
    SOURCE_FILES src/TransformSystem.cpp
)

engine_target_add_static_type_info(
    NAME          system_transform_configuration_codegen
    TARGET        system_transform
    SOURCE_FILE   ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformSystem.cpp
    HEADERS       ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/TransformSystem.hpp
    LOGICAL_PATHS lux/engine/scene/TransformSystem.hpp
)

component_include_directories(
    system_transform
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${PROJECT_SOURCE_DIR}/engine/foundation/ecs/hierarchy/sinclude
    INSTALL_TIME include
)

target_compile_definitions(
    system_transform
    PRIVATE LUX_ENGINE_SCENE_TRANSFORM_LIBRARY
)

target_link_libraries(
    system_transform
    PUBLIC
        lux::engine::scene::scene_composition
        lux::engine::ecs::ecs_core
        lux::engine::ecs::ecs_hierarchy
        lux::engine::ecs::ecs_transform
)

component_add_transitive_commands(
    system_transform
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_core ecs_hierarchy ecs_transform)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-transform
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS system_transform
)

lux_classify_target(TARGET system_transform LAYER SYSTEMS PRODUCT RUNTIME ROLE LIBRARY)
