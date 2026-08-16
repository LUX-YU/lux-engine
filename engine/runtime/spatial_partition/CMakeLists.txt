generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL_PARTITION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL_PARTITION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_SPATIAL_PARTITION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/spatial_partition/visibility.h
)

add_component(
    COMPONENT_NAME runtime_spatial_partition
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_spatial_partition
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SpatialDemandPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SpatialPartitionSystem.cpp
)

component_include_directories(
    runtime_spatial_partition
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_spatial_partition
    PRIVATE LUX_ENGINE_RUNTIME_SPATIAL_PARTITION_LIBRARY
)

target_link_libraries(
    runtime_spatial_partition
    PUBLIC
        lux::engine::ecs::core
        lux::engine::resource::entity_scene
        lux::engine::runtime::runtime_entity_scene
)

component_add_transitive_commands(
    runtime_spatial_partition
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS entity_scene)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene)"
)

lux_classify_target(
    TARGET  runtime_spatial_partition
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    spatial_demand_planner_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial_demand_planner_test.cpp
)
target_link_libraries(
    spatial_demand_planner_test
    PRIVATE lux::engine::runtime::runtime_spatial_partition
)
if(MSVC)
    target_compile_options(spatial_demand_planner_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial_demand_planner_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial_demand_planner_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME spatial_demand_planner_test COMMAND spatial_demand_planner_test)
set_tests_properties(
    spatial_demand_planner_test
    PROPERTIES LABELS "cpu;runtime;spatial_partition" TIMEOUT 30
)

add_executable(
    spatial_partition_loader_integration_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial_partition_loader_integration_test.cpp
)
target_link_libraries(
    spatial_partition_loader_integration_test
    PRIVATE
        lux::engine::runtime::runtime_spatial_partition
        lux::engine::runtime::runtime_execution
)
if(MSVC)
    target_compile_options(
        spatial_partition_loader_integration_test PRIVATE /UNDEBUG
    )
else()
    target_compile_options(
        spatial_partition_loader_integration_test PRIVATE -UNDEBUG
    )
endif()
lux_classify_target(
    TARGET  spatial_partition_loader_integration_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME spatial_partition_loader_integration_test
    COMMAND spatial_partition_loader_integration_test
)
set_tests_properties(
    spatial_partition_loader_integration_test
    PROPERTIES LABELS "cpu;runtime;spatial_partition;async" TIMEOUT 30
)
