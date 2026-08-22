add_executable(
    spatial3d_partition_headless_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial3d_partition_headless_test.cpp
)
target_link_libraries(
    spatial3d_partition_headless_test
    PRIVATE
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_execution
        lux::engine::core::serialization
        lux::engine::ecs::spatial3d_streaming
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
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::spatial3d_streaming
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
        LUX_ENGINE_RUNTIME_SPATIAL3D_STREAMING_SYSTEMS_LIBRARY
    PUBLIC_MACRO_NAME
        LUX_ENGINE_RUNTIME_SPATIAL3D_STREAMING_SYSTEMS_PUBLIC
    DISABLE_DLL_MACRO_NAME
        LUX_ENGINE_RUNTIME_SPATIAL3D_STREAMING_SYSTEMS_DLL_DISABLE
    GENERATE_FILE_PATH
        lux/engine/runtime/spatial3d/partitioned/systems_visibility.h
)

add_component(
    COMPONENT_NAME runtime_spatial3d_streaming_systems
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_spatial3d_streaming_systems
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/InstallSpatial3DStreamingSystems.cpp
)

component_include_directories(
    runtime_spatial3d_streaming_systems
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_spatial3d_streaming_systems
    PRIVATE
        LUX_ENGINE_RUNTIME_SPATIAL3D_STREAMING_SYSTEMS_LIBRARY
)

target_link_libraries(
    runtime_spatial3d_streaming_systems
    PUBLIC
        lux::engine::runtime::runtime_entity_scene
        lux::engine::ecs::entity_scene
        lux::engine::ecs::spatial3d_streaming
        lux::engine::ecs::transform
        lux::engine::spatial3d::scene_catalog
        lux::engine::ecs::core
)

component_add_transitive_commands(
    runtime_spatial3d_streaming_systems
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core entity_scene spatial3d_streaming transform)"
    "find_package(lux-engine-spatial3d REQUIRED COMPONENTS scene_catalog)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene)"
)

lux_classify_target(
    TARGET  runtime_spatial3d_streaming_systems
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)
