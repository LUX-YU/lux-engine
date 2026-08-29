find_package(Eigen3 CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_TRANSFORM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_TRANSFORM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_ECS_TRANSFORM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/ecs/transform/visibility.h
)

add_component(
    COMPONENT_NAME transform
    NAMESPACE      lux::engine::simulation::ecs
    OUTPUT_NAME    lux_engine_simulation_ecs_transform
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformSchema.cpp
)

component_include_directories(
    transform
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    transform
    PRIVATE LUX_ENGINE_SIMULATION_ECS_TRANSFORM_LIBRARY
)
target_link_libraries(
    transform
    PUBLIC
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::schema
        lux::engine::simulation::ecs::snapshot
        lux::engine::core::math
        lux::engine::core::serialization
)
component_add_internal_dependencies(transform core schema snapshot)
component_add_transitive_commands(
    transform
    "find_package(Eigen3 CONFIG REQUIRED)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math serialization)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core schema snapshot)"
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME transform_component_codegen
    TARGET transform
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp
    SYMBOL transform
)

lux_classify_target(
    TARGET  transform
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(transform_decode_test test/transform_decode_test.cpp)
target_link_libraries(
    transform_decode_test
    PRIVATE
        lux::engine::simulation::ecs::transform
        lux::engine::simulation::ecs::hierarchy
)
if(MSVC)
    target_compile_options(transform_decode_test PRIVATE /UNDEBUG)
else()
    target_compile_options(transform_decode_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  transform_decode_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME vnext_transform_decode COMMAND transform_decode_test)
set_tests_properties(
    vnext_transform_decode
    PROPERTIES LABELS "vnext;l1;transform;schema;cpu"
)
