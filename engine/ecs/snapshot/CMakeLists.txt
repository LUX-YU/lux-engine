find_package(EnTT CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_SNAPSHOT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_SNAPSHOT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_SNAPSHOT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/snapshot/visibility.h
)

add_component(
    COMPONENT_NAME snapshot
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_snapshot
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ComponentSnapshotSet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EcsSnapshot.cpp
)

component_include_directories(
    snapshot
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    snapshot
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../schema/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(snapshot PRIVATE LUX_ENGINE_ECS_SNAPSHOT_LIBRARY)
target_link_libraries(
    snapshot
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::schema
        lux::cxx::compile_time
        EnTT::EnTT
)
component_add_internal_dependencies(snapshot core schema)
component_add_transitive_commands(
    snapshot
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core schema)"
)

lux_classify_target(
    TARGET  snapshot
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(ecs_snapshot_test test/snapshot_test.cpp)
target_include_directories(
    ecs_snapshot_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/sinclude
)
target_compile_definitions(
    ecs_snapshot_test
    PRIVATE LUX_ECS_SNAPSHOT_TESTING=1
)
target_link_libraries(
    ecs_snapshot_test
    PRIVATE
        lux::engine::ecs::snapshot
        lux::engine::ecs::core
)
if(MSVC)
    target_compile_options(ecs_snapshot_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_snapshot_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_snapshot_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_snapshot COMMAND ecs_snapshot_test)
set_tests_properties(vnext_ecs_snapshot PROPERTIES LABELS "vnext;l1;snapshot;cpu")
