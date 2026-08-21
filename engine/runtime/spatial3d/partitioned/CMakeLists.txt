generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/spatial3d/partitioned/visibility.h
)

add_component(
    COMPONENT_NAME runtime_spatial3d_partitioned
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_spatial3d_partitioned
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DSectionSource.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SpatialInterest3DSystem.cpp
)

component_include_directories(
    runtime_spatial3d_partitioned
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_spatial3d_partitioned
    PRIVATE LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_LIBRARY
)

target_link_libraries(
    runtime_spatial3d_partitioned
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::scene_format
        lux::engine::ecs::spatial3d_components
        lux::engine::ecs::transform
        lux::engine::scene::scene
        lux::engine::core::math
        lux::engine::runtime::runtime_spatial_partition
        lux::cxx::compile_time
)

component_add_transitive_commands(
    runtime_spatial3d_partitioned
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core scene_format spatial3d_components transform)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_spatial_partition)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  runtime_spatial3d_partitioned
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    spatial3d_interest_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial3d_interest_test.cpp
)
target_link_libraries(
    spatial3d_interest_test
    PRIVATE lux::engine::runtime::runtime_spatial3d_partitioned
)
if(MSVC)
    target_compile_options(spatial3d_interest_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial3d_interest_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial3d_interest_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    DOMAIN
)

add_executable(
    spatial3d_partition_headless_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial3d_partition_headless_test.cpp
)
target_link_libraries(
    spatial3d_partition_headless_test
    PRIVATE
        lux::engine::runtime::runtime_spatial3d_partitioned
        lux::engine::runtime::runtime_execution
        lux::engine::core::serialization
)
if(MSVC)
    target_compile_options(spatial3d_partition_headless_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial3d_partition_headless_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial3d_partition_headless_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)

add_executable(
    spatial3d_scale_invariance_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial3d_scale_invariance_test.cpp
)
target_link_libraries(
    spatial3d_scale_invariance_test
    PRIVATE
        lux::engine::runtime::runtime_spatial3d_partitioned
        lux::engine::runtime::runtime_execution
)
if(MSVC)
    target_compile_options(spatial3d_scale_invariance_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial3d_scale_invariance_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial3d_scale_invariance_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)

generate_visibility_header(
    ENABLE_MACRO_NAME
        LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_CONTRIBUTION_LIBRARY
    PUBLIC_MACRO_NAME
        LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_CONTRIBUTION_PUBLIC
    DISABLE_DLL_MACRO_NAME
        LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_CONTRIBUTION_DLL_DISABLE
    GENERATE_FILE_PATH
        lux/engine/runtime/spatial3d/partitioned/contribution_visibility.h
)

add_component(
    COMPONENT_NAME runtime_pack_spatial3d_partitioned
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_pack_spatial3d_partitioned
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DPartitionedContribution.cpp
)

component_include_directories(
    runtime_pack_spatial3d_partitioned
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_pack_spatial3d_partitioned
    PRIVATE
        LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_CONTRIBUTION_LIBRARY
)

target_link_libraries(
    runtime_pack_spatial3d_partitioned
    PUBLIC
        lux::engine::runtime::runtime_spatial3d_partitioned
        lux::engine::runtime::runtime_spatial_partition
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::runtime::runtime_pack_spatial3d_transform
        lux::engine::spatial3d::scene_catalog
        lux::engine::ecs::core
)

component_add_transitive_commands(
    runtime_pack_spatial3d_partitioned
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-spatial3d REQUIRED COMPONENTS scene_catalog)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_scene_contributions runtime_pack_spatial3d_transform runtime_spatial_partition runtime_spatial3d_partitioned)"
)

lux_classify_target(
    TARGET  runtime_pack_spatial3d_partitioned
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)
