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
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ComponentLoadSet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSectionImage.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSectionLoader.cpp
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

add_executable(
    ecs_world_section_image_test
    test/WorldSectionFixtureBuilder.cpp
    test/world_section_image_test.cpp
)
target_include_directories(
    ecs_world_section_image_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
)
target_link_libraries(
    ecs_world_section_image_test
    PRIVATE lux::engine::ecs::world_section
)
if(MSVC)
    target_compile_options(ecs_world_section_image_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_world_section_image_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_world_section_image_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME vnext_ecs_world_section_image
    COMMAND ecs_world_section_image_test
)
set_tests_properties(
    vnext_ecs_world_section_image
    PROPERTIES LABELS "vnext;l1;world-section;cpu"
)

add_executable(
    ecs_component_load_set_test
    test/component_load_set_test.cpp
)
target_compile_definitions(
    ecs_component_load_set_test
    PRIVATE LUX_ECS_WORLD_SECTION_TESTING=1
)
target_link_libraries(
    ecs_component_load_set_test
    PRIVATE lux::engine::ecs::world_section
)
if(MSVC)
    target_compile_options(ecs_component_load_set_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_component_load_set_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_component_load_set_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME vnext_ecs_component_load_set
    COMMAND ecs_component_load_set_test
)
set_tests_properties(
    vnext_ecs_component_load_set
    PROPERTIES LABELS "vnext;l1;world-section;schema;cpu"
)

add_executable(
    ecs_world_section_loader_test
    test/WorldSectionFixtureBuilder.cpp
    test/world_section_loader_test.cpp
)
target_include_directories(
    ecs_world_section_loader_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
)
target_compile_definitions(
    ecs_world_section_loader_test
    PRIVATE LUX_ECS_WORLD_SECTION_TESTING=1
)
target_link_libraries(
    ecs_world_section_loader_test
    PRIVATE
        lux::engine::ecs::world_section
        lux::engine::ecs::schedule
)
if(MSVC)
    target_compile_options(ecs_world_section_loader_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_world_section_loader_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_world_section_loader_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME vnext_ecs_world_section_loader
    COMMAND ecs_world_section_loader_test
)
set_tests_properties(
    vnext_ecs_world_section_loader
    PROPERTIES LABELS "vnext;l1;world-section;load;cpu"
)
