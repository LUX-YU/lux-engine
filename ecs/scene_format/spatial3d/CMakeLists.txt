# Cooked Spatial3D cell/LOD/Section index and stable L3SC v1 codec.
# Streaming behavior and product composition do not belong to this target.

find_package(lux-cxx REQUIRED COMPONENTS compile_time core)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ECS_SPATIAL3D_SCENE_FORMAT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ECS_SPATIAL3D_SCENE_FORMAT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ECS_SPATIAL3D_SCENE_FORMAT_DLL_DISABLE
    GENERATE_FILE_PATH
        lux/engine/ecs/scene_format/spatial3d/visibility.h
)

add_component(
    COMPONENT_NAME spatial3d_scene_format
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_spatial3d_scene_format
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneCatalog.cpp
)

component_include_directories(
    spatial3d_scene_format
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    spatial3d_scene_format
    PRIVATE LUX_ECS_SPATIAL3D_SCENE_FORMAT_LIBRARY
)

target_link_libraries(
    spatial3d_scene_format
    PUBLIC
        lux::cxx::compile_time
        lux::cxx::core
        lux::engine::ecs::scene_format
        lux::engine::core::math
    PRIVATE
        lux::engine::core::serialization
)

component_add_transitive_commands(
    spatial3d_scene_format
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time core)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math serialization)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS scene_format)"
)

lux_classify_target(
    TARGET  spatial3d_scene_format
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    spatial3d_scene_catalog_public_link_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/scene_catalog_public_link_test.cpp
)
target_link_libraries(
    spatial3d_scene_catalog_public_link_test
    PRIVATE lux::engine::ecs::spatial3d_scene_format
)
if(MSVC)
    target_compile_options(
        spatial3d_scene_catalog_public_link_test PRIVATE /UNDEBUG)
else()
    target_compile_options(
        spatial3d_scene_catalog_public_link_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial3d_scene_catalog_public_link_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)

add_executable(
    spatial3d_scene_catalog_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/scene_catalog_contract_test.cpp
)
target_link_libraries(
    spatial3d_scene_catalog_contract_test
    PRIVATE lux::engine::ecs::spatial3d_scene_format
)
if(MSVC)
    target_compile_options(
        spatial3d_scene_catalog_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(
        spatial3d_scene_catalog_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial3d_scene_catalog_contract_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)
