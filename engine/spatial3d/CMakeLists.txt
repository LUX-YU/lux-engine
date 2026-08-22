# Engine-owned Spatial3D scene catalog and L3SC v1 codec.

find_package(lux-cxx REQUIRED COMPONENTS compile_time core)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SPATIAL3D_SCENE_CATALOG_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SPATIAL3D_SCENE_CATALOG_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SPATIAL3D_SCENE_CATALOG_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/spatial3d/visibility.h
)

add_component(
    COMPONENT_NAME scene_catalog
    NAMESPACE      lux::engine::spatial3d
    OUTPUT_NAME    lux_engine_spatial3d_scene_catalog
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneCatalog.cpp
)

component_include_directories(
    scene_catalog
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    scene_catalog
    PRIVATE LUX_ENGINE_SPATIAL3D_SCENE_CATALOG_LIBRARY
)

target_link_libraries(
    scene_catalog
    PUBLIC
        lux::cxx::compile_time
        lux::cxx::core
        lux::engine::ecs::scene_format
        lux::engine::scene::scene
        lux::engine::core::math
    PRIVATE
        lux::engine::core::serialization
)

component_add_transitive_commands(
    scene_catalog
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time core)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math serialization)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS scene_format)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene)"
)

lux_classify_target(
    TARGET  scene_catalog
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

# Public-link probe: verifies DLL exports and the installed public dependency
# closure without linking the legacy Resource component.
add_executable(
    spatial3d_scene_catalog_public_link_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/scene_catalog_public_link_test.cpp
)
target_link_libraries(
    spatial3d_scene_catalog_public_link_test
    PRIVATE lux::engine::spatial3d::scene_catalog
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
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)

add_executable(
    spatial3d_scene_catalog_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/scene_catalog_contract_test.cpp
)
target_link_libraries(
    spatial3d_scene_catalog_contract_test
    PRIVATE lux::engine::spatial3d::scene_catalog
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
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)

install_components(
    PROJECT_NAME lux-engine-spatial3d
    VERSION      0.0.1
    NAMESPACE    lux::engine::spatial3d
    COMPONENTS   scene_catalog
)
