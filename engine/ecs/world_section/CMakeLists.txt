find_package(stduuid CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_WORLD_SECTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_WORLD_SECTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/world_section/visibility.h
)

add_component(
    COMPONENT_NAME world_section
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_world_section
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSection.cpp
)

component_include_directories(
    world_section
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    world_section
    PRIVATE LUX_ENGINE_ECS_WORLD_SECTION_LIBRARY
)
target_link_libraries(
    world_section
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::schema
        lux::engine::core::serialization
        stduuid
)
component_add_internal_dependencies(world_section core schema)
component_add_transitive_commands(
    world_section
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core schema)"
)

lux_classify_target(
    TARGET  world_section
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    ecs_world_section_contract_test
    test/world_section_contract_test.cpp
)
target_link_libraries(
    ecs_world_section_contract_test
    PRIVATE lux::engine::ecs::world_section
)
if(MSVC)
    target_compile_options(ecs_world_section_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_world_section_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_world_section_contract_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME vnext_ecs_world_section_contract
    COMMAND ecs_world_section_contract_test
)
set_tests_properties(
    vnext_ecs_world_section_contract
    PROPERTIES LABELS "vnext;l1;world-section;contract;cpu"
)
