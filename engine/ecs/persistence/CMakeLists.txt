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
)

component_include_directories(
    persistence
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    persistence
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(persistence PRIVATE LUX_ENGINE_ECS_PERSISTENCE_LIBRARY)
target_link_libraries(
    persistence
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::schema
        lux::engine::ecs::snapshot
        lux::engine::ecs::world_section
        lux::cxx::compile_time
        stduuid
)
component_add_internal_dependencies(
    persistence
    core
    schema
    snapshot
    world_section
)
component_add_transitive_commands(
    persistence
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core schema snapshot world_section)"
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME persistence_component_codegen
    TARGET persistence
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/PersistentEntity.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/PersistentEntity.hpp
    LOGICAL_PATH lux/engine/ecs/PersistentEntity.hpp
    SYMBOL persistence
)

lux_classify_target(
    TARGET  persistence
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(ecs_persistence_test test/persistence_test.cpp)
target_compile_definitions(
    ecs_persistence_test
    PRIVATE LUX_ECS_PERSISTENCE_TESTING=1
)
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
