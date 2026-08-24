find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_PERSISTENCE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_PERSISTENCE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_PERSISTENCE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/persistence/visibility.h
)

add_component(
    COMPONENT_NAME persistence
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_persistence
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/PersistentEntity.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TaggedPropertyArchive.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSection.cpp
)

component_include_directories(
    persistence
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(persistence PRIVATE LUX_ENGINE_ECS_PERSISTENCE_LIBRARY)
target_link_libraries(
    persistence
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::schema
        lux::engine::core::serialization
        lux::cxx::compile_time
        stduuid
)
component_add_internal_dependencies(persistence core schema)
component_add_transitive_commands(
    persistence
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core schema)"
)

lux_classify_target(
    TARGET  persistence
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(ecs_persistence_test test/persistence_test.cpp)
target_link_libraries(ecs_persistence_test PRIVATE lux::engine::ecs::persistence)
if(MSVC)
    target_compile_options(ecs_persistence_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_persistence_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_persistence_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_persistence COMMAND ecs_persistence_test)
set_tests_properties(vnext_ecs_persistence PROPERTIES LABELS "vnext;l1;persistence;cpu")
