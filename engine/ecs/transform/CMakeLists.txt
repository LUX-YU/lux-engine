find_package(Eigen3 CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_TRANSFORM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_TRANSFORM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_TRANSFORM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/transform/visibility.h
)

add_component(
    COMPONENT_NAME transform
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_transform
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformSchema.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformSystem.cpp
)

component_include_directories(
    transform
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/sinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(
    transform
    PRIVATE LUX_ENGINE_ECS_TRANSFORM_LIBRARY
)
target_link_libraries(
    transform
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::schedule
        lux::engine::ecs::schema
        lux::engine::ecs::hierarchy
        lux::engine::ecs::world_section
        lux::engine::core::math
)
component_add_internal_dependencies(
    transform core schedule schema hierarchy world_section
)
component_add_transitive_commands(
    transform
    "find_package(Eigen3 CONFIG REQUIRED)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core schedule schema hierarchy world_section)"
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME transform_component_codegen
    TARGET transform
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/ecs/Transform.hpp
    SYMBOL transform
)

lux_classify_target(
    TARGET  transform
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(ecs_transform_test test/transform_test.cpp)
target_include_directories(
    ecs_transform_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_link_libraries(
    ecs_transform_test
    PRIVATE
        lux::engine::ecs::transform
        lux::engine::ecs::snapshot
        lux::engine::ecs::persistence
)
if(MSVC)
    target_compile_options(ecs_transform_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_transform_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_transform_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME vnext_ecs_transform COMMAND ecs_transform_test)
set_tests_properties(
    vnext_ecs_transform
    PROPERTIES LABELS "vnext;l1;transform;cpu"
)
