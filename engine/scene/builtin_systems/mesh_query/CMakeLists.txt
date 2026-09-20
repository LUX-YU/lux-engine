generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SCENE_MESH_QUERY_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SCENE_MESH_QUERY_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/scene/mesh_query/visibility.h)

add_component(COMPONENT_NAME scene_mesh_query NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_mesh_query SOURCE_FILES src/MeshQuerySystem.cpp)
component_include_directories(scene_mesh_query
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_compile_definitions(scene_mesh_query PRIVATE LUX_ENGINE_SCENE_MESH_QUERY_LIBRARY)
target_link_libraries(scene_mesh_query PUBLIC
    lux::engine::scene::scene_composition
    lux::engine::simulation::ecs::transform lux::engine::simulation::ecs::visual
    lux::engine::core::math lux::engine::resource::identity)
component_add_transitive_commands(scene_mesh_query
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS transform visual)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)")
lux_engine_install_components(PROJECT_NAME lux-engine-scene-mesh-query
    VERSION ${PROJECT_VERSION} NAMESPACE lux::engine::scene COMPONENTS scene_mesh_query)

if(BUILD_TESTING)
    add_executable(scene_mesh_query_test test/mesh_query.cpp)
    target_link_libraries(scene_mesh_query_test PRIVATE lux::engine::scene::scene_mesh_query)
    if(MSVC)
        target_compile_options(scene_mesh_query_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_mesh_query_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.mesh_query COMMAND scene_mesh_query_test)
endif()
