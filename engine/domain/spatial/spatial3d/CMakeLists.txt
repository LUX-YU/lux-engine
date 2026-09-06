find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SPATIAL3D_INDEX_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SPATIAL3D_INDEX_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SPATIAL3D_INDEX_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/spatial/spatial3d/visibility.h
)

add_component(
    COMPONENT_NAME spatial3d_index
    NAMESPACE      lux::engine::spatial
    OUTPUT_NAME    lux_engine_spatial3d_index
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DPartitionIndex.cpp
)
component_include_directories(
    spatial3d_index
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(spatial3d_index PRIVATE LUX_ENGINE_SPATIAL3D_INDEX_LIBRARY)
target_link_libraries(
    spatial3d_index
    PUBLIC
        lux::engine::core::math
        lux::engine::partition::identity
        lux::cxx::compile_time
)
component_add_transitive_commands(
    spatial3d_index
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-partition-identity REQUIRED COMPONENTS partition_identity)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)
lux_classify_target(
    TARGET  spatial3d_index
    LAYER   DOMAIN
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

install_components(
    PROJECT_NAME lux-engine-domain-spatial3d
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::spatial
    COMPONENTS   spatial3d_index
)
