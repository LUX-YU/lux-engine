find_package(EnTT CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_SNAPSHOT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_SNAPSHOT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_SNAPSHOT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/snapshot/visibility.h
)

add_component(
    COMPONENT_NAME ecs_snapshot
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_snapshot
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ComponentSnapshotSet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EcsSnapshot.cpp
)

component_include_directories(
    ecs_snapshot
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    ecs_snapshot
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(ecs_snapshot PRIVATE LUX_ENGINE_ECS_SNAPSHOT_LIBRARY)
target_link_libraries(
    ecs_snapshot
    PUBLIC
        lux::engine::ecs::ecs_core
        lux::engine::ecs::ecs_schema
        lux::cxx::compile_time
        EnTT::EnTT
)
component_add_internal_dependencies(ecs_snapshot ecs_core ecs_schema)
component_add_transitive_commands(
    ecs_snapshot
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_core ecs_schema)"
)
