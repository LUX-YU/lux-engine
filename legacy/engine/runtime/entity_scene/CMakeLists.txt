find_package(EnTT CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time memory)
if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()
find_package(zstd CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_ENTITY_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_ENTITY_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/entity_scene/visibility.h
)

add_component(
    COMPONENT_NAME runtime_entity_scene
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_entity_scene
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SectionBlobStore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionGeneratorCatalog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionService.cpp
)

component_include_directories(
    runtime_entity_scene
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(
    runtime_entity_scene
    PRIVATE LUX_ENGINE_RUNTIME_ENTITY_SCENE_LIBRARY
)

target_link_libraries(
    runtime_entity_scene
    PUBLIC
        lux::cxx::compile_time
        lux::cxx::memory
        lux::engine::ecs::entity_scene
        lux::engine::ecs::scene_format
        lux::engine::scene::scene
        lux::engine::resource::asset
        lux::engine::runtime::runtime_execution
        lux::engine::scene::scene
        STDEXEC::stdexec
    PRIVATE
        zstd::libzstd
)

component_add_transitive_commands(
    runtime_entity_scene
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time memory)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS entity_scene scene_format)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution)"
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(zstd CONFIG REQUIRED)"
)

lux_classify_target(
    TARGET  runtime_entity_scene
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)
