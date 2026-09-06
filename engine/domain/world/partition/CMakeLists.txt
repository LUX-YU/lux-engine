find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_WORLD_PARTITION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_WORLD_PARTITION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_WORLD_PARTITION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/world/partition/visibility.h
)

add_component(
    COMPONENT_NAME world_partition
    NAMESPACE      lux::engine::world
    OUTPUT_NAME    lux_engine_world_partition
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldPartition.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldPartitionFailureInjection.cpp
)
add_library(lux::engine::world::partition ALIAS world_partition)
component_include_directories(
    world_partition
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(world_partition PRIVATE LUX_ENGINE_WORLD_PARTITION_LIBRARY)
target_link_libraries(
    world_partition
    PUBLIC
        lux::engine::world::identity
        lux::engine::partition::identity
        lux::cxx::compile_time
)
component_add_transitive_commands(
    world_partition
    "find_package(lux-engine-world-identity REQUIRED COMPONENTS world_identity)"
    "find_package(lux-engine-partition-identity REQUIRED COMPONENTS partition_identity)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "add_library(lux::engine::world::partition INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::world::partition PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::world::world_partition)"
)
lux_classify_target(TARGET world_partition LAYER WORLD PRODUCT RUNTIME ROLE FOUNDATION)

install_components(
    PROJECT_NAME lux-engine-world-partition
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::world
    COMPONENTS   world_partition
)
