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
        lux::cxx::core
        lux::cxx::compile_time
)
component_add_internal_dependencies(schema core)
component_add_transitive_commands(
    schema
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
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

add_test(
    NAME vnext_ecs_missing_component_policy_rejected
    COMMAND
        ${CMAKE_COMMAND}
        -DHEADER=${CMAKE_CURRENT_SOURCE_DIR}/test/missing_component_policy.hpp
        -DVALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_ecs_component_annotations.cmake
        -P ${CMAKE_CURRENT_SOURCE_DIR}/test/ExpectMissingPolicyFailure.cmake
)
set_tests_properties(
    vnext_ecs_missing_component_policy_rejected
    PROPERTIES
        LABELS "vnext;l1;schema;negative;codegen"
)

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
