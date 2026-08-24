find_package(lux-cxx REQUIRED COMPONENTS core compile_time reflection_runtime)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_SCHEMA_REFLECTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_SCHEMA_REFLECTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_SCHEMA_REFLECTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/schema_reflection/visibility.h
)

add_component(
    COMPONENT_NAME schema_reflection
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_schema_reflection
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ReflectedComponentCodec.cpp
)

component_include_directories(
    schema_reflection
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    schema_reflection
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../schema/pinclude
)

target_compile_definitions(
    schema_reflection
    PRIVATE LUX_ENGINE_ECS_SCHEMA_REFLECTION_LIBRARY
)
target_link_libraries(
    schema_reflection
    PUBLIC
        lux::engine::ecs::schema
        lux::engine::core::meta
        lux::cxx::reflection_runtime
)
component_add_internal_dependencies(schema_reflection core schema)
component_add_transitive_commands(
    schema_reflection
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time reflection_runtime)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core schema)"
)

lux_classify_target(
    TARGET  schema_reflection
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    ecs_schema_test
    ${CMAKE_CURRENT_SOURCE_DIR}/../schema/test/schema_test.cpp
)
include_component_cmake_scripts(meta)
engine_add_meta(
    NAME ecs_schema_test_meta
    TARGET_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../schema/test/GeneratedSchemaProbe.hpp
    REGISTER_FUNC_NAME lux_ecs_schema_probe
)
target_link_libraries(
    ecs_schema_test
    PRIVATE lux::engine::ecs::schema_reflection
)
target_include_directories(
    ecs_schema_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../schema/test
)
engine_target_add_meta(
    TARGET ecs_schema_test
    METAS ecs_schema_test_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/schema_test_meta
)
if(MSVC)
    target_compile_options(ecs_schema_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_schema_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_schema_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_schema COMMAND ecs_schema_test)
set_tests_properties(
    vnext_ecs_schema
    PROPERTIES LABELS "vnext;l1;schema;reflection;cpu"
)
