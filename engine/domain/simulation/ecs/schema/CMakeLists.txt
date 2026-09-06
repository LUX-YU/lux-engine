find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_SCHEMA_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_SCHEMA_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_ECS_SCHEMA_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/ecs/schema/visibility.h
)

add_component(
    COMPONENT_NAME schema
    NAMESPACE      lux::engine::simulation::ecs
    OUTPUT_NAME    lux_engine_simulation_ecs_schema
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ComponentSchemaSet.cpp
)

component_include_directories(
    schema
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    schema
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(schema PRIVATE LUX_ENGINE_SIMULATION_ECS_SCHEMA_LIBRARY)
target_link_libraries(
    schema
    PUBLIC
        lux::engine::simulation::ecs::core
        lux::engine::core::serialization
        lux::engine::core::type_info
        lux::cxx::core
        lux::cxx::compile_time
)
component_add_internal_dependencies(schema core)
component_add_transitive_commands(
    schema
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization type_info)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
)

lux_classify_target(
    TARGET  schema
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

include_component_cmake_scripts(meta)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_ecs_codegen.cmake)
