find_package(EnTT CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time memory)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_ENTITY_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_ENTITY_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_ENTITY_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/entity_scene/visibility.h
)

add_component(
    COMPONENT_NAME entity_scene
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_entity_scene
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityBatchDecoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/PreparedEntityBatch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityBatchStager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityBatchMaterializer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionLoaderSystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/StartupSectionSystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SectionResidencyPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionResidencySystem.cpp
)

component_include_directories(
    entity_scene
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(
    entity_scene
    PRIVATE LUX_ENGINE_ECS_ENTITY_SCENE_LIBRARY
)

target_link_libraries(
    entity_scene
    PUBLIC
        lux::cxx::compile_time
        lux::cxx::memory
        lux::engine::core::async_port
        lux::engine::ecs::core
        lux::engine::ecs::scene_format
        lux::engine::resource::asset
        EnTT::EnTT
    PRIVATE
        lux::engine::core::meta
        lux::engine::core::serialization
        lux::engine::ecs::component_archive
)

component_add_transitive_commands(
    entity_scene
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS async_port meta serialization)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core scene_format component_archive)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
)

lux_classify_target(
    TARGET  entity_scene
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    section_residency_planner_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/section_residency_planner_test.cpp
)
target_link_libraries(
    section_residency_planner_test
    PRIVATE lux::engine::ecs::entity_scene
)
if(MSVC)
    target_compile_options(section_residency_planner_test PRIVATE /UNDEBUG)
else()
    target_compile_options(section_residency_planner_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  section_residency_planner_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)
