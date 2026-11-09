generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SCENE_MESH_QUERY_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SCENE_MESH_QUERY_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/scene/mesh_query/visibility.h
)

add_component(
    COMPONENT_NAME system_mesh_query
    NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_system_mesh_query
    SOURCE_FILES src/MeshQuerySystem.cpp src/MeshQuery.cpp
)

component_include_directories(
    system_mesh_query
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(system_mesh_query PRIVATE LUX_ENGINE_SCENE_MESH_QUERY_LIBRARY)

target_link_libraries(
    system_mesh_query
    PUBLIC
        lux::engine::scene::scene_composition
        lux::engine::ecs::ecs_transform
        lux::engine::ecs::ecs_visual
        lux::engine::core::math
        lux::engine::resource::identity
    PRIVATE lux::engine::resource::description
)

component_add_transitive_commands(
    system_mesh_query
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_transform ecs_visual)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity description)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-mesh-query
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS system_mesh_query
)

lux_classify_target(TARGET system_mesh_query LAYER SYSTEMS PRODUCT RUNTIME ROLE LIBRARY)
