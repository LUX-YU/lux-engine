find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RESOURCE_SPATIAL3D_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RESOURCE_SPATIAL3D_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RESOURCE_SPATIAL3D_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/spatial3d_scene/visibility.h
)

add_component(
    COMPONENT_NAME spatial3d_scene
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_resource_spatial3d_scene
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DSceneCatalog.cpp
)

component_include_directories(
    spatial3d_scene
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    spatial3d_scene
    PRIVATE LUX_ENGINE_RESOURCE_SPATIAL3D_SCENE_LIBRARY
)

target_link_libraries(
    spatial3d_scene
    PUBLIC
        lux::cxx::compile_time
        lux::engine::resource::entity_scene
        lux::engine::resource::spatial
    PRIVATE lux::engine::core::serialization
)

component_add_transitive_commands(
    spatial3d_scene
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS entity_scene spatial)"
)

lux_classify_target(
    TARGET  spatial3d_scene
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    spatial3d_scene_catalog_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial3d_scene_catalog_test.cpp
)
target_link_libraries(
    spatial3d_scene_catalog_test
    PRIVATE lux::engine::resource::spatial3d_scene
)
if(MSVC)
    target_compile_options(spatial3d_scene_catalog_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial3d_scene_catalog_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial3d_scene_catalog_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)
