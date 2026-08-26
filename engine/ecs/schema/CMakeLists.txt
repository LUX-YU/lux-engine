find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_SCHEMA_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_SCHEMA_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_SCHEMA_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/schema/visibility.h
)

add_component(
    COMPONENT_NAME schema
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_schema
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

target_compile_definitions(schema PRIVATE LUX_ENGINE_ECS_SCHEMA_LIBRARY)
target_link_libraries(
    schema
    PUBLIC
        lux::engine::ecs::core
        lux::engine::core::type_info
        lux::cxx::core
        lux::cxx::compile_time
)
component_add_internal_dependencies(schema core)
component_add_transitive_commands(
    schema
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS type_info)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
)

lux_classify_target(
    TARGET  schema
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(ecs_schema_test test/schema_test.cpp)
target_link_libraries(ecs_schema_test PRIVATE lux::engine::ecs::schema)
if(MSVC)
    target_compile_options(ecs_schema_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_schema_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET ecs_schema_test
    LAYER ECS
    PRODUCT TEST
    ROLE FOUNDATION
)
add_test(NAME vnext_ecs_schema COMMAND ecs_schema_test)
set_tests_properties(vnext_ecs_schema PROPERTIES LABELS "vnext;l1;schema;cpu")

include_component_cmake_scripts(meta)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_ecs_codegen.cmake)
foreach(policy_probe IN ITEMS
    missing_component_policy
    duplicate_component_policy
    invalid_component_policy
)
    add_library(
        ecs_${policy_probe}_negative
        STATIC EXCLUDE_FROM_ALL
        test/${policy_probe}_negative.cpp
    )
    target_include_directories(
        ecs_${policy_probe}_negative
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
    )
    target_link_libraries(
        ecs_${policy_probe}_negative
        PRIVATE lux::engine::ecs::schema
    )
    engine_target_add_ecs_component_codegen(
        NAME ${policy_probe}_codegen
        TARGET ecs_${policy_probe}_negative
        SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/test/${policy_probe}_negative.cpp
        HEADER ${CMAKE_CURRENT_SOURCE_DIR}/test/${policy_probe}.hpp
        LOGICAL_PATH test/${policy_probe}.hpp
        SYMBOL ${policy_probe}
    )
    lux_classify_target(
        TARGET  ecs_${policy_probe}_negative
        LAYER   ECS
        PRODUCT TEST
        ROLE    FOUNDATION
    )
    add_test(
        NAME vnext_ecs_${policy_probe}_rejected
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=ecs_${policy_probe}_negative
            -DEXPECTED_PATTERN=${policy_probe}
            -P ${PROJECT_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )
    set_tests_properties(
        vnext_ecs_${policy_probe}_rejected
        PROPERTIES
            LABELS "vnext;l1;schema;negative;codegen"
    )
endforeach()

foreach(probe IN ITEMS
    component_operations_mutable_get_negative
    component_operations_storage_key_negative
    component_operations_clone_negative
)
    add_executable(
        ecs_${probe} EXCLUDE_FROM_ALL
        test/${probe}.cpp
    )
    target_link_libraries(ecs_${probe} PRIVATE lux::engine::ecs::schema)
    lux_classify_target(
        TARGET  ecs_${probe}
        LAYER   ECS
        PRODUCT TEST
        ROLE    FOUNDATION
    )
    add_test(
        NAME vnext_ecs_${probe}
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=ecs_${probe}
            "-DEXPECTED_PATTERN=${probe}"
            -P ${PROJECT_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )
    set_tests_properties(
        vnext_ecs_${probe}
        PROPERTIES LABELS "vnext;l1;schema;negative"
    )
endforeach()
