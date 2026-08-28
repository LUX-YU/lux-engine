generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_TRANSFORM_SYSTEM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_TRANSFORM_SYSTEM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_TRANSFORM_SYSTEM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/systems/transform/visibility.h
)

add_component(
    COMPONENT_NAME transform_system
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_transform_system
    SOURCE_FILES src/TransformSystem.cpp
)

component_include_directories(
    transform_system
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)

target_compile_definitions(
    transform_system
    PRIVATE LUX_ENGINE_SIMULATION_TRANSFORM_SYSTEM_LIBRARY
)

target_link_libraries(
    transform_system
    PUBLIC
        lux::engine::simulation::system
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::hierarchy
        lux::engine::simulation::ecs::transform
)

component_add_transitive_commands(
    transform_system
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core hierarchy transform)"
)

lux_classify_target(
    TARGET  transform_system
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(transform_system_test test/transform_system_test.cpp)
target_include_directories(
    transform_system_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../../ecs/hierarchy/sinclude
)
target_link_libraries(
    transform_system_test
    PRIVATE
        lux::engine::simulation::transform_system
        lux::engine::simulation::ecs::snapshot
)
if(MSVC)
    target_compile_options(transform_system_test PRIVATE /UNDEBUG)
else()
    target_compile_options(transform_system_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  transform_system_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME vnext_transform_system COMMAND transform_system_test)
set_tests_properties(
    vnext_transform_system
    PROPERTIES LABELS "vnext;l1;transform;cpu"
)
