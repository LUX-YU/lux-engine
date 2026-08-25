find_package(lux-cxx REQUIRED COMPONENTS compile_time container)

option(
    LUX_ECS_CONTRACT_CHECKS
    "Enable ECS execution contract checks in all build configurations"
    OFF
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_SYSTEM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_SYSTEM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_SYSTEM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/system/visibility.h
)

add_component(
    COMPONENT_NAME system
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_system
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ChangeSet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SystemExecution.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SystemRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SystemRelations.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SystemTaskGraphCompiler.cpp
)

component_include_directories(
    system
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/sinclude
    INSTALL_TIME include
)

target_compile_definitions(system PRIVATE LUX_ENGINE_ECS_SYSTEM_LIBRARY)
if(LUX_ECS_CONTRACT_CHECKS)
    target_compile_definitions(system PUBLIC LUX_ECS_CONTRACT_CHECKS)
endif()
target_link_libraries(
    system
    PUBLIC
        lux::engine::ecs::core
        lux::engine::core::task
        lux::cxx::compile_time
        lux::cxx::container
)
component_add_internal_dependencies(system core)
component_add_transitive_commands(
    system
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time container)"
    "find_package(lux-engine-core REQUIRED COMPONENTS task)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
)

lux_classify_target(
    TARGET  system
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(ecs_system_registry_test test/system_registry_test.cpp)
target_link_libraries(ecs_system_registry_test PRIVATE lux::engine::ecs::system)
if(MSVC)
    target_compile_options(ecs_system_registry_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_system_registry_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_system_registry_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_system_registry COMMAND ecs_system_registry_test)
set_tests_properties(
    vnext_ecs_system_registry
    PROPERTIES LABELS "vnext;l1;system;cpu"
)

add_executable(
    ecs_system_execution_test
    test/system_execution_test.cpp
)
target_link_libraries(
    ecs_system_execution_test
    PRIVATE lux::engine::ecs::system
)
if(MSVC)
    target_compile_options(ecs_system_execution_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_system_execution_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_system_execution_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_system_execution COMMAND ecs_system_execution_test)
set_tests_properties(
    vnext_ecs_system_execution
    PROPERTIES LABELS "vnext;l1;system;taskgraph;cpu"
)

foreach(probe IN ITEMS
    registry_wrong_thread
    registry_execution_mutation
)
    add_executable(
        ecs_${probe}_probe
        test/${probe}_probe.cpp
    )
    target_link_libraries(
        ecs_${probe}_probe
        PRIVATE lux::engine::ecs::system
    )
    lux_classify_target(
        TARGET  ecs_${probe}_probe
        LAYER   ECS
        PRODUCT TEST
        ROLE    FOUNDATION
    )
    add_test(
        NAME vnext_ecs_${probe}_rejected
        COMMAND
            ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:ecs_${probe}_probe>
            -P ${PROJECT_SOURCE_DIR}/cmake/ExpectProcessFailure.cmake
    )
    set_tests_properties(
        vnext_ecs_${probe}_rejected
        PROPERTIES LABELS "vnext;l1;system;contract;negative"
    )
endforeach()

foreach(probe IN ITEMS
    system_missing_access_negative
    system_throwing_update_negative
    system_duplicate_access_negative
    retired_schedule_header_negative
    retired_system_frame_header_negative
    retired_system_base_negative
    retired_system_set_header_negative
    retired_system_start_header_negative
    retired_system_compiler_class_negative
    retired_system_compilation_fields_negative
    retired_task_wave_backend_negative
    retired_bare_system_id_negative
    retired_context_update_negative
    untrusted_system_access_negative
    world_schedule_backref_negative
)
    add_executable(ecs_${probe} EXCLUDE_FROM_ALL test/${probe}.cpp)
    target_link_libraries(ecs_${probe} PRIVATE lux::engine::ecs::system)
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
        PROPERTIES LABELS "vnext;l1;system;negative"
    )
endforeach()

include_component_cmake_scripts(meta)
engine_add_type_runtime_info(
    NAME ecs_object_boundary_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/test/ObjectBoundaryProbe.hpp
    REGISTER_FUNC_NAME lux_ecs_object_boundary_probe
)
add_executable(
    ecs_object_boundary_test
    test/object_boundary_test.cpp
)
target_include_directories(
    ecs_object_boundary_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_link_libraries(
    ecs_object_boundary_test
    PRIVATE
        lux::engine::ecs::system
        lux::engine::core::object
)
if(MSVC)
    target_compile_options(ecs_object_boundary_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_object_boundary_test PRIVATE -UNDEBUG)
endif()
engine_target_add_type_runtime_info(
    TARGET ecs_object_boundary_test
    METAS ecs_object_boundary_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/object_boundary_meta
)
lux_classify_target(
    TARGET  ecs_object_boundary_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_object_boundary COMMAND ecs_object_boundary_test)
set_tests_properties(
    vnext_ecs_object_boundary
    PROPERTIES LABELS "vnext;l1;system;object;cpu"
)
