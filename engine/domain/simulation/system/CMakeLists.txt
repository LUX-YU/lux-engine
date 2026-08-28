find_package(lux-cxx REQUIRED COMPONENTS compile_time container)
find_package(EnTT CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SYSTEM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SYSTEM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SYSTEM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/system/visibility.h
)

add_component(
    COMPONENT_NAME system
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_system
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SystemRegistry.cpp
)

component_include_directories(
    system
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(system PRIVATE LUX_ENGINE_SIMULATION_SYSTEM_LIBRARY)
target_link_libraries(
    system
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::ecs::core
        EnTT::EnTT
        lux::cxx::compile_time
        lux::cxx::container
)
component_add_transitive_commands(
    system
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time container)"
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
)

lux_classify_target(
    TARGET  system
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(ecs_system_registry_test test/system_registry_test.cpp)
target_link_libraries(
    ecs_system_registry_test
    PRIVATE
        lux::engine::simulation::system
        lux::engine::simulation::ecs::task
        lux::engine::core::task
)
if(MSVC)
    target_compile_options(ecs_system_registry_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_system_registry_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_system_registry_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_system_registry COMMAND ecs_system_registry_test)

add_executable(
    simulation_endpoint_point_test
    test/endpoint_point_test.cpp
)
target_link_libraries(
    simulation_endpoint_point_test
    PRIVATE
        lux::engine::simulation::system
        lux::engine::core::task
)
if(MSVC)
    target_compile_options(simulation_endpoint_point_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_endpoint_point_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_endpoint_point_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME simulation_endpoint_point_test
    COMMAND simulation_endpoint_point_test
)

add_executable(
    simulation_throwing_event_payload_negative EXCLUDE_FROM_ALL
    test/throwing_event_payload_negative.cpp
)
target_link_libraries(
    simulation_throwing_event_payload_negative
    PRIVATE lux::engine::simulation::system
)
lux_classify_target(
    TARGET  simulation_throwing_event_payload_negative
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME simulation_throwing_event_payload_rejected
    COMMAND
        ${CMAKE_COMMAND}
        -DBUILD_DIR=${CMAKE_BINARY_DIR}
        -DTARGET=simulation_throwing_event_payload_negative
        -DEXPECTED_PATTERN=throwing_event_payload_negative
        -P ${PROJECT_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
)
set_tests_properties(
    simulation_throwing_event_payload_rejected
    PROPERTIES LABELS "l1;simulation;negative;exception"
)

include_component_cmake_scripts(meta)
engine_add_type_runtime_info(
    NAME simulation_object_boundary_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/test/ObjectBoundaryProbe.hpp
    REGISTER_FUNC_NAME lux_simulation_object_boundary
)
add_executable(
    simulation_object_boundary_test
    test/object_boundary_test.cpp
)
target_include_directories(
    simulation_object_boundary_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
)
target_link_libraries(
    simulation_object_boundary_test
    PRIVATE
        lux::engine::simulation::system
        lux::engine::simulation::ecs::task
        lux::engine::core::object
        lux::engine::core::task
)
engine_target_add_type_runtime_info(
    TARGET simulation_object_boundary_test
    METAS simulation_object_boundary_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/object_boundary_meta
)
if(MSVC)
    target_compile_options(simulation_object_boundary_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_object_boundary_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_object_boundary_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(
    NAME simulation_object_boundary_test
    COMMAND simulation_object_boundary_test
)

install_components(
    PROJECT_NAME lux-engine-simulation-system
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   system
)
