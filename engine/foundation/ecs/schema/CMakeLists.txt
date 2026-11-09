find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_SCHEMA_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_SCHEMA_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_SCHEMA_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/schema/visibility.h
)

add_component(
    COMPONENT_NAME ecs_schema
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_schema
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ComponentSchemaSet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/DecodedComponent.cpp
)

component_include_directories(
    ecs_schema
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME include
)
target_include_directories(
    ecs_schema
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
)

target_compile_definitions(ecs_schema PRIVATE LUX_ENGINE_ECS_SCHEMA_LIBRARY)
target_link_libraries(
    ecs_schema
    PUBLIC
        lux::engine::ecs::ecs_core
        lux::engine::core::serialization
        lux::engine::core::type_info
        lux::engine::world::world_identity
        lux::cxx::core
        lux::cxx::compile_time
)
component_add_internal_dependencies(ecs_schema ecs_core)
component_add_transitive_commands(
    ecs_schema
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization type_info)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_core)"
    "find_package(lux-engine-world-identity REQUIRED COMPONENTS world_identity)"
)

include_component_cmake_scripts(meta)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_ecs_codegen.cmake)
component_add_cmake_scripts(ecs_schema ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_ecs_codegen.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/template/
    DESTINATION share/lux-engine-ecs/schema/template)
