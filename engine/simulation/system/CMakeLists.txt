find_package(lux-cxx REQUIRED COMPONENTS compile_time container)

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
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/../ecs/core/sinclude
    INSTALL_TIME include
)

target_compile_definitions(system PRIVATE LUX_ENGINE_SIMULATION_SYSTEM_LIBRARY)
target_link_libraries(
    system
    PUBLIC
        lux::engine::simulation::ecs::core
        lux::cxx::compile_time
        lux::cxx::container
)
component_add_transitive_commands(
    system
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time container)"
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

install_components(
    PROJECT_NAME lux-engine-simulation-system
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   system
)
